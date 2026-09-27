// Note: for StreamSocket, the client uses a server socket, the server uses a client socket.
// This is because of certificate management. The server needs to trust a client and its certificate
//
// StreamSender and StreamReceiver endpoints allow for convenient conversion of the header to/from
// bytes while still handling the additional byte buffer with zero copies and extra allocations.

// Performance analysis:
// We want to minimize the transmission time for various sizes of packets.
// The current code locks the write socket *per shard* and not *per packet*. This leds to the best
// performance outcome given that the possible packets can be either very small (one shard) or very
// large (hundreds/thousands of shards, for video). if we don't allow interleaving shards, a very
// small packet will need to wait a long time before getting received if there was an ongoing
// transmission of a big packet before. If we allow interleaving shards, small packets can be
// transmitted quicker, with only minimal latency increase for the ongoing transmission of the big
// packet.
// Note: We can't clone the underlying socket for each StreamSender and the mutex around the socket
// cannot be removed. This is because we need to make sure at least shards are written whole.

use crate::backend::{tcp, udp, SocketReader, SocketWriter};
use alvr_common::{
    anyhow::Result, debug, parking_lot::Mutex, AnyhowToCon, ConResult, HandleTryAgain, ToCon,
};
use alvr_session::{DscpTos, SocketBufferSize, SocketProtocol};
use serde::{de::DeserializeOwned, Serialize};
use std::{
    cmp::Ordering,
    collections::{HashMap, HashSet},
    marker::PhantomData,
    mem,
    net::{IpAddr, TcpListener, UdpSocket},
    sync::{mpsc, Arc},
    time::Duration,
};

pub const SHARD_PREFIX_SIZE: usize = mem::size_of::<u32>() // packet length - field itself (4 bytes)
    + mem::size_of::<u16>() // stream ID
    + mem::size_of::<u32>() // packet index
    + mem::size_of::<u32>() // shards count
    + mem::size_of::<u32>(); // shards index

/// Memory buffer that contains a hidden prefix
#[derive(Default)]
pub struct Buffer<H = ()> {
    inner: Vec<u8>,
    hidden_offset: usize, // this corresponds to prefix + header
    length: usize,
    _phantom: PhantomData<H>,
}

impl<H> Buffer<H> {
    /// Length of payload (without prefix)
    #[must_use]
    pub fn len(&self) -> usize {
        self.length
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Get the whole payload of the buffer
    pub fn get(&self) -> &[u8] {
        &self.inner[self.hidden_offset..][..self.length]
    }

    /// If the range is outside the valid range, new space will be allocated
    /// NB: the offset parameter is applied on top of the internal offset of the buffer
    pub fn get_range_mut(&mut self, offset: usize, size: usize) -> &mut [u8] {
        let required_size = self.hidden_offset + offset + size;
        if required_size > self.inner.len() {
            self.inner.resize(required_size, 0);
        }

        self.length = self.length.max(offset + size);

        &mut self.inner[self.hidden_offset + offset..][..size]
    }

    /// If length > current length, allocate more space
    pub fn set_len(&mut self, length: usize) {
        self.inner.resize(self.hidden_offset + length, 0);
        self.length = length;
    }
}

#[derive(Clone)]
pub struct StreamSender<H> {
    inner: Arc<Mutex<Box<dyn SocketWriter>>>,
    stream_id: u16,
    max_packet_size: usize,
    // if the packet index overflows the worst that happens is a false positive packet loss
    next_packet_index: u32,
    used_buffers: Vec<Vec<u8>>,
    // Segmented sends: datagrams laid out back to back for one send call, and their lengths.
    staging: Vec<u8>,
    staged_lengths: Vec<usize>,
    _phantom: PhantomData<H>,
}

impl<H> StreamSender<H> {
    pub fn send(&mut self, buffer: Buffer<H>) -> Result<()> {
        self.send_many(std::iter::once(buffer))
    }

    /// Sends several packets in order. With UDP segmentation offload (Windows, see
    /// set_udp_send_segmentation) their datagrams go to the stack up to ~64 KB per call, across
    /// packet boundaries: a PyroWave frame is thousands of one-datagram packets, and one call per
    /// datagram is what capped the streamer's send rate. Every datagram of a call but the last
    /// must be exactly max_packet_size, so a shorter one (the tail of a packet) ends the call.
    /// Without segmentation each packet is sent the zero-copy way, one call per datagram.
    pub fn send_many(&mut self, buffers: impl IntoIterator<Item = Buffer<H>>) -> Result<()> {
        // A whole number of datagrams below the 64 KB UDP limit.
        let datagrams_per_call = usize::max(1, 65_000 / self.max_packet_size);

        // With datagrams of more than half of that (e.g. a 32 KB packet size) a call carries one
        // datagram either way, and staging would only add a copy of every byte.
        if !crate::udp_send_segmentation_wanted() || datagrams_per_call == 1 {
            for buffer in buffers {
                self.send_in_place(buffer)?;
            }
            return Ok(());
        }
        let max_shard_data_size = self.max_packet_size - SHARD_PREFIX_SIZE;

        for buffer in buffers {
            let actual_buffer_size = buffer.hidden_offset + buffer.length;
            let data_size = actual_buffer_size - SHARD_PREFIX_SIZE;
            let shards_count = data_size.div_ceil(max_shard_data_size).max(1);

            for idx in 0..shards_count {
                let start = idx * max_shard_data_size;
                let packet_length = usize::min(self.max_packet_size, actual_buffer_size - start);

                self.staging.extend_from_slice(
                    &((packet_length - mem::size_of::<u32>()) as u32).to_be_bytes(),
                );
                self.staging
                    .extend_from_slice(&self.stream_id.to_be_bytes());
                self.staging
                    .extend_from_slice(&self.next_packet_index.to_be_bytes());
                self.staging
                    .extend_from_slice(&(shards_count as u32).to_be_bytes());
                self.staging.extend_from_slice(&(idx as u32).to_be_bytes());
                self.staging.extend_from_slice(
                    &buffer.inner[start + SHARD_PREFIX_SIZE..start + packet_length],
                );
                self.staged_lengths.push(packet_length);

                if packet_length != self.max_packet_size
                    || self.staged_lengths.len() == datagrams_per_call
                {
                    self.flush_staging()?;
                }
            }

            self.next_packet_index += 1;
            self.used_buffers.push(buffer.inner);
        }

        self.flush_staging()
    }

    fn flush_staging(&mut self) -> Result<()> {
        if self.staged_lengths.is_empty() {
            return Ok(());
        }
        let staging = mem::take(&mut self.staging);
        let lengths = mem::take(&mut self.staged_lengths);

        let result = (|| {
            let mut socket = self.inner.lock();
            if lengths.len() > 1 && socket.send_segmented(&staging, self.max_packet_size)? {
                return Ok(());
            }
            // One datagram, TCP, or refused by Windows: one call each.
            let mut offset = 0;
            for length in &lengths {
                socket.send(&staging[offset..offset + length])?;
                offset += length;
            }
            Ok(())
        })();

        // Keep the allocations for the next call.
        self.staging = staging;
        self.staging.clear();
        self.staged_lengths = lengths;
        self.staged_lengths.clear();

        result
    }

    /// Shard and send a buffer with zero copies and zero allocations.
    /// The prefix of each shard is written over the previously sent shard to avoid reallocations.
    fn send_in_place(&mut self, mut buffer: Buffer<H>) -> Result<()> {
        let max_shard_data_size = self.max_packet_size - SHARD_PREFIX_SIZE;
        let actual_buffer_size = buffer.hidden_offset + buffer.length;
        let data_size = actual_buffer_size - SHARD_PREFIX_SIZE;
        let shards_count = (data_size as f32 / max_shard_data_size as f32).ceil() as usize;

        for idx in 0..shards_count {
            // this overlaps with the previous shard, this is intended behavior and allows to
            // reduce allocations
            let packet_start_position = idx * max_shard_data_size;
            let sub_buffer = &mut buffer.inner[packet_start_position..];

            // NB: true shard length (account for last shard that is smaller)
            let packet_length = usize::min(
                self.max_packet_size,
                actual_buffer_size - packet_start_position,
            );

            // todo: switch to little endian
            // todo: do not remove sizeof<u32> for packet length
            sub_buffer[0..4]
                .copy_from_slice(&((packet_length - mem::size_of::<u32>()) as u32).to_be_bytes());
            sub_buffer[4..6].copy_from_slice(&self.stream_id.to_be_bytes());
            sub_buffer[6..10].copy_from_slice(&self.next_packet_index.to_be_bytes());
            sub_buffer[10..14].copy_from_slice(&(shards_count as u32).to_be_bytes());
            sub_buffer[14..18].copy_from_slice(&(idx as u32).to_be_bytes());

            self.inner.lock().send(&sub_buffer[..packet_length])?;
        }

        self.next_packet_index += 1;

        self.used_buffers.push(buffer.inner);

        Ok(())
    }
}

impl<H: Serialize> StreamSender<H> {
    pub fn get_buffer(&mut self, header: &H) -> Result<Buffer<H>> {
        let mut buffer = self.used_buffers.pop().unwrap_or_default();

        let header_size = bincode::serialized_size(header)? as usize;
        let hidden_offset = SHARD_PREFIX_SIZE + header_size;

        if buffer.len() < hidden_offset {
            buffer.resize(hidden_offset, 0);
        }

        bincode::serialize_into(&mut buffer[SHARD_PREFIX_SIZE..hidden_offset], header)?;

        Ok(Buffer {
            inner: buffer,
            hidden_offset,
            length: 0,
            _phantom: PhantomData,
        })
    }

    pub fn send_header(&mut self, header: &H) -> Result<()> {
        let buffer = self.get_buffer(header)?;
        self.send(buffer)
    }
}

pub struct ReceiverData<H> {
    buffer: Option<Vec<u8>>,
    size: usize, // counting the prefix
    used_buffer_queue: mpsc::Sender<Vec<u8>>,
    had_packet_loss: bool,
    _phantom: PhantomData<H>,
}

impl<H> ReceiverData<H> {
    pub fn had_packet_loss(&self) -> bool {
        self.had_packet_loss
    }
}

impl<H: DeserializeOwned> ReceiverData<H> {
    pub fn get(&self) -> Result<(H, &[u8])> {
        let mut data: &[u8] = &self.buffer.as_ref().unwrap()[SHARD_PREFIX_SIZE..self.size];
        // This will partially consume the slice, leaving only the actual payload
        let header = bincode::deserialize_from(&mut data)?;

        Ok((header, data))
    }
    pub fn get_header(&self) -> Result<H> {
        Ok(self.get()?.0)
    }
}

impl<H> Drop for ReceiverData<H> {
    fn drop(&mut self) {
        self.used_buffer_queue
            .send(self.buffer.take().unwrap())
            .ok();
    }
}

struct ReconstructedPacket {
    index: u32,
    buffer: Vec<u8>,
    size: usize, // contains prefix
}

pub struct StreamReceiver<H> {
    packet_receiver: mpsc::Receiver<ReconstructedPacket>,
    used_buffer_queue: mpsc::Sender<Vec<u8>>,
    last_packet_index: Option<u32>,
    _phantom: PhantomData<H>,
}

fn wrapping_cmp(lhs: u32, rhs: u32) -> Ordering {
    let diff = lhs.wrapping_sub(rhs);
    if diff == 0 {
        Ordering::Equal
    } else if diff < u32::MAX / 2 {
        Ordering::Greater
    } else {
        // if diff > u32::MAX / 2, it means the sub operation wrapped
        Ordering::Less
    }
}

/// Get next packet reconstructing from shards.
/// Returns true if a packet has been recontructed and copied into the buffer.
impl<H: DeserializeOwned + Serialize> StreamReceiver<H> {
    pub fn recv(&mut self, timeout: Duration) -> ConResult<ReceiverData<H>> {
        let packet = self
            .packet_receiver
            .recv_timeout(timeout)
            .handle_try_again()?;

        let mut had_packet_loss = false;

        if let Some(last_idx) = self.last_packet_index {
            // Use wrapping arithmetics
            match wrapping_cmp(packet.index, last_idx.wrapping_add(1)) {
                Ordering::Equal => (),
                Ordering::Greater => {
                    // Skipped some indices
                    had_packet_loss = true
                }
                Ordering::Less => {
                    // Old packet, discard
                    self.used_buffer_queue.send(packet.buffer).to_con()?;
                    return alvr_common::try_again();
                }
            }
        }
        self.last_packet_index = Some(packet.index);

        Ok(ReceiverData {
            buffer: Some(packet.buffer),
            size: packet.size,
            used_buffer_queue: self.used_buffer_queue.clone(),
            had_packet_loss,
            _phantom: PhantomData,
        })
    }
}

pub enum StreamSocketBuilder {
    Tcp(TcpListener),
    Udp(UdpSocket),
}

impl StreamSocketBuilder {
    pub fn listen_for_server(
        timeout: Duration,
        port: u16,
        stream_socket_config: SocketProtocol,
        stream_tos_config: Option<DscpTos>,
        send_buffer_bytes: SocketBufferSize,
        recv_buffer_bytes: SocketBufferSize,
    ) -> Result<Self> {
        Ok(match stream_socket_config {
            SocketProtocol::Udp => StreamSocketBuilder::Udp(udp::bind(
                port,
                stream_tos_config,
                send_buffer_bytes,
                recv_buffer_bytes,
            )?),
            SocketProtocol::Tcp => StreamSocketBuilder::Tcp(tcp::bind(
                timeout,
                port,
                stream_tos_config,
                send_buffer_bytes,
                recv_buffer_bytes,
            )?),
        })
    }

    pub fn accept_from_server(
        self,
        server_ip: IpAddr,
        port: u16,
        max_packet_size: usize,
        timeout: Duration,
    ) -> ConResult<StreamSocket> {
        let (send_socket, receive_socket): (Box<dyn SocketWriter>, Box<dyn SocketReader>) =
            match self {
                StreamSocketBuilder::Udp(socket) => {
                    let (send_socket, receive_socket) =
                        udp::connect(&socket, server_ip, port, timeout).to_con()?;

                    (Box::new(send_socket), Box::new(receive_socket))
                }
                StreamSocketBuilder::Tcp(listener) => {
                    let (send_socket, receive_socket) =
                        tcp::accept_from_server(&listener, Some(server_ip), timeout)?;

                    (Box::new(send_socket), Box::new(receive_socket))
                }
            };

        Ok(StreamSocket {
            // +4 is a workaround to retain compatibilty with old protocol
            // todo: remove +4
            max_packet_size: max_packet_size + 4,
            send_socket: Arc::new(Mutex::new(send_socket)),
            receive_socket,
            shard_recv_state: None,
            datagram_scratch: Vec::new(),
            stream_recv_components: HashMap::new(),
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub fn connect_to_client(
        timeout: Duration,
        client_ip: IpAddr,
        port: u16,
        protocol: SocketProtocol,
        dscp: Option<DscpTos>,
        send_buffer_bytes: SocketBufferSize,
        recv_buffer_bytes: SocketBufferSize,
        max_packet_size: usize,
    ) -> ConResult<StreamSocket> {
        let (send_socket, receive_socket): (Box<dyn SocketWriter>, Box<dyn SocketReader>) =
            match protocol {
                SocketProtocol::Udp => {
                    let socket =
                        udp::bind(port, dscp, send_buffer_bytes, recv_buffer_bytes).to_con()?;
                    let (send_socket, receive_socket) =
                        udp::connect(&socket, client_ip, port, timeout).to_con()?;

                    (Box::new(send_socket), Box::new(receive_socket))
                }
                SocketProtocol::Tcp => {
                    let (send_socket, receive_socket) = tcp::connect_to_client(
                        timeout,
                        &[client_ip],
                        port,
                        send_buffer_bytes,
                        recv_buffer_bytes,
                    )?;

                    (Box::new(send_socket), Box::new(receive_socket))
                }
            };

        Ok(StreamSocket {
            // +4 is a workaround to retain compatibilty with old protocol
            // todo: remove +4
            max_packet_size: max_packet_size + 4,
            send_socket: Arc::new(Mutex::new(send_socket)),
            receive_socket,
            shard_recv_state: None,
            datagram_scratch: Vec::new(),
            stream_recv_components: HashMap::new(),
        })
    }
}

struct RecvState {
    shard_length: usize, // contains prefix length itself
    stream_id: u16,
    packet_index: u32,
    shards_count: usize,
    shard_index: usize,
    packet_cursor: usize, // counts also the prefix bytes
    overwritten_data_backup: Option<[u8; SHARD_PREFIX_SIZE]>,
    should_discard: bool,
}

struct InProgressPacket {
    buffer: Vec<u8>,
    buffer_length: usize,
    received_shard_indices: HashSet<usize>,
}

struct StreamRecvComponents {
    used_buffer_sender: mpsc::Sender<Vec<u8>>,
    used_buffer_receiver: mpsc::Receiver<Vec<u8>>,
    packet_queue: mpsc::Sender<ReconstructedPacket>,
    in_progress_packets: HashMap<u32, InProgressPacket>,
    discarded_shards_sink: InProgressPacket,
}

// Note: used buffers don't *have* to be split by stream ID, but doing so improves memory usage
// todo: impose cap on number of created buffers to avoid OOM crashes
pub struct StreamSocket {
    max_packet_size: usize,
    send_socket: Arc<Mutex<Box<dyn SocketWriter>>>,
    receive_socket: Box<dyn SocketReader>,
    shard_recv_state: Option<RecvState>,
    stream_recv_components: HashMap<u16, StreamRecvComponents>,
    // UDP only: the whole shard of the current recv(), read in one call. See recv().
    datagram_scratch: Vec<u8>,
}

impl StreamSocket {
    pub fn request_stream<T>(&self, stream_id: u16) -> StreamSender<T> {
        StreamSender {
            inner: Arc::clone(&self.send_socket),
            stream_id,
            max_packet_size: self.max_packet_size,
            next_packet_index: 0,
            used_buffers: vec![],
            staging: vec![],
            staged_lengths: vec![],
            _phantom: PhantomData,
        }
    }

    // max_concurrent_buffers: number of buffers allocated by this call which will be reused to
    // receive packets for this stream ID. If packets are not read fast enough, the shards received
    // for this particular stream will be discarded
    pub fn subscribe_to_stream<T>(
        &mut self,
        stream_id: u16,
        max_concurrent_buffers: usize,
    ) -> StreamReceiver<T> {
        let (packet_sender, packet_receiver) = mpsc::channel();
        let (used_buffer_sender, used_buffer_receiver) = mpsc::channel();

        for _ in 0..max_concurrent_buffers {
            used_buffer_sender.send(vec![]).ok();
        }

        self.stream_recv_components.insert(
            stream_id,
            StreamRecvComponents {
                used_buffer_sender: used_buffer_sender.clone(),
                used_buffer_receiver,
                packet_queue: packet_sender,
                in_progress_packets: HashMap::new(),
                discarded_shards_sink: InProgressPacket {
                    buffer: vec![],
                    buffer_length: 0,
                    received_shard_indices: HashSet::new(),
                },
            },
        );

        StreamReceiver {
            packet_receiver,
            used_buffer_queue: used_buffer_sender,
            _phantom: PhantomData,
            last_packet_index: None,
        }
    }

    pub fn recv(&mut self) -> ConResult {
        // UDP: one syscall per shard. The stream path below peeks at the 18-byte prefix first and
        // then reads the datagram a second time straight into its place in the packet buffer,
        // which saves a 1.4 KB copy at the price of a whole extra syscall per shard. With
        // PyroWave every video packet is a datagram, about 180,000 a second at 90 fps; the JPEG XS
        // client saw its receive thread fall behind at 113,000 a second on the two-call path
        // (kernel buffer full, 5-11 % of the packets dropped). A UDP datagram always arrives
        // whole, so none of the partial-read state is needed either.
        let datagram = self.receive_socket.is_datagram();

        let shard_recv_state_mut = if let Some(state) = &mut self.shard_recv_state {
            state
        } else {
            let mut bytes = [0; SHARD_PREFIX_SIZE];
            if datagram {
                if self.datagram_scratch.len() < self.max_packet_size {
                    self.datagram_scratch.resize(self.max_packet_size, 0);
                }
                let count = self.receive_socket.recv(&mut self.datagram_scratch)?;
                if count < SHARD_PREFIX_SIZE {
                    return alvr_common::try_again();
                }
                bytes.copy_from_slice(&self.datagram_scratch[..SHARD_PREFIX_SIZE]);
                let declared = mem::size_of::<u32>()
                    + u32::from_be_bytes(bytes[0..4].try_into().unwrap()) as usize;
                if declared != count {
                    // Truncated or not one of ours: drop it rather than misplace its bytes.
                    return alvr_common::try_again();
                }
            } else {
                let count = self.receive_socket.peek(&mut bytes)?;
                if count < SHARD_PREFIX_SIZE {
                    return alvr_common::try_again();
                }
            }

            // todo: switch to little endian
            // todo: do not remove sizeof<u32> for packet length
            let shard_length = mem::size_of::<u32>()
                + u32::from_be_bytes(bytes[0..4].try_into().unwrap()) as usize;
            let stream_id = u16::from_be_bytes(bytes[4..6].try_into().unwrap());
            let packet_index = u32::from_be_bytes(bytes[6..10].try_into().unwrap());
            let shards_count = u32::from_be_bytes(bytes[10..14].try_into().unwrap()) as usize;
            let shard_index = u32::from_be_bytes(bytes[14..18].try_into().unwrap()) as usize;

            self.shard_recv_state.insert(RecvState {
                shard_length,
                stream_id,
                packet_index,
                shards_count,
                shard_index,
                packet_cursor: 0,
                overwritten_data_backup: None,
                should_discard: false,
            })
        };

        let Some(components) = self
            .stream_recv_components
            .get_mut(&shard_recv_state_mut.stream_id)
        else {
            debug!(
                "Received packet from stream {} before subscribing!",
                shard_recv_state_mut.stream_id
            );
            if datagram {
                // Already consumed from the socket, so there is nothing to retry: drop it.
                self.shard_recv_state = None;
            }
            return alvr_common::try_again();
        };

        let in_progress_packet = if shard_recv_state_mut.should_discard {
            &mut components.discarded_shards_sink
        } else if let Some(packet) = components
            .in_progress_packets
            .get_mut(&shard_recv_state_mut.packet_index)
        {
            packet
        } else if let Some(buffer) = components.used_buffer_receiver.try_recv().ok().or_else(|| {
            // By default, try to dequeue a used buffer. In case none were found, recycle one of the
            // in progress packets, chances are these buffers are "dead" because one of their shards
            // has been dropped by the network.
            let idx = *components.in_progress_packets.iter().next()?.0;
            Some(components.in_progress_packets.remove(&idx).unwrap().buffer)
        }) {
            // NB: Can't use entry pattern because we want to allow bailing out on the line above
            components.in_progress_packets.insert(
                shard_recv_state_mut.packet_index,
                InProgressPacket {
                    buffer,
                    buffer_length: 0,
                    // todo: find a way to skipping this allocation
                    received_shard_indices: HashSet::with_capacity(
                        shard_recv_state_mut.shards_count,
                    ),
                },
            );
            components
                .in_progress_packets
                .get_mut(&shard_recv_state_mut.packet_index)
                .unwrap()
        } else {
            // This branch may be hit in case the thread related to the stream hangs for some reason
            shard_recv_state_mut.should_discard = true;
            shard_recv_state_mut.packet_cursor = 0; // reset cursor from old shards
                                                    // always write at the start of the packet so the buffer doesn't grow much
            shard_recv_state_mut.shard_index = 0;

            &mut components.discarded_shards_sink
        };

        let max_shard_data_size = self.max_packet_size - SHARD_PREFIX_SIZE;
        // Note: there is no prefix offset, since we want to write the prefix too.
        let packet_start_index = shard_recv_state_mut.shard_index * max_shard_data_size;

        // Prepare buffer to accomodate receiving shard
        {
            // Note: this contains the prefix offset
            in_progress_packet.buffer_length = usize::max(
                in_progress_packet.buffer_length,
                packet_start_index + shard_recv_state_mut.shard_length,
            );

            if in_progress_packet.buffer.len() < in_progress_packet.buffer_length {
                in_progress_packet
                    .buffer
                    .resize(in_progress_packet.buffer_length, 0);
            }
        }

        let sub_buffer = &mut in_progress_packet.buffer[packet_start_index..];

        // Read shard into the single contiguous buffer
        if datagram {
            // Already read. The prefix is not copied: only the data is ever read back out
            // (ReceiverData::get starts at SHARD_PREFIX_SIZE), so unlike the stream path there is
            // nothing to overwrite and nothing to restore.
            let len = shard_recv_state_mut.shard_length;
            sub_buffer[SHARD_PREFIX_SIZE..len]
                .copy_from_slice(&self.datagram_scratch[SHARD_PREFIX_SIZE..len]);
        } else {
            // Backup the small section of bytes that will be overwritten by reading from socket.
            if shard_recv_state_mut.overwritten_data_backup.is_none() {
                shard_recv_state_mut.overwritten_data_backup =
                    Some(sub_buffer[..SHARD_PREFIX_SIZE].try_into().unwrap())
            }

            // This loop may bail out at any time if a timeout is reached. This is correctly handled by
            // the previous code.
            while shard_recv_state_mut.packet_cursor < shard_recv_state_mut.shard_length {
                let size = self.receive_socket.recv(
                    &mut sub_buffer
                        [shard_recv_state_mut.packet_cursor..shard_recv_state_mut.shard_length],
                )?;
                shard_recv_state_mut.packet_cursor += size;
            }

            // Restore backed up bytes
            // Safety: overwritten_data_backup is always set just before receiving the packet
            sub_buffer[..SHARD_PREFIX_SIZE]
                .copy_from_slice(&shard_recv_state_mut.overwritten_data_backup.take().unwrap());
        }

        if !shard_recv_state_mut.should_discard {
            in_progress_packet
                .received_shard_indices
                .insert(shard_recv_state_mut.shard_index);
        }

        // Check if packet is complete and send
        if in_progress_packet.received_shard_indices.len() == shard_recv_state_mut.shards_count {
            let size = in_progress_packet.buffer_length;
            components
                .packet_queue
                .send(ReconstructedPacket {
                    index: shard_recv_state_mut.packet_index,
                    buffer: components
                        .in_progress_packets
                        .remove(&shard_recv_state_mut.packet_index)
                        .unwrap()
                        .buffer,
                    size,
                })
                .ok();

            // Keep only shards with later packet index (using wrapping logic)
            while let Some((idx, _)) = components.in_progress_packets.iter().find(|(idx, _)| {
                wrapping_cmp(**idx, shard_recv_state_mut.packet_index) == Ordering::Less
            }) {
                let idx = *idx; // fix borrow rule
                let packet = components.in_progress_packets.remove(&idx).unwrap();

                // Recycle buffer
                components.used_buffer_sender.send(packet.buffer).ok();
            }
        }

        // Mark current shard as read and allow for a new shard to be read
        self.shard_recv_state = None;

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{USO_OFF, USO_STATE, USO_WANTED};
    use alvr_common::ConnectionError;
    use std::collections::VecDeque;

    // What goes out on the wire. With `segmented`, it accepts segmented sends and cuts them the way
    // UDP segmentation offload does, checking the rule it imposes: every datagram of a call but the
    // last is exactly segment_size.
    struct Wire {
        datagrams: Arc<Mutex<Vec<Vec<u8>>>>,
        calls: Arc<Mutex<usize>>,
        segmented: bool,
    }

    impl SocketWriter for Wire {
        fn send(&mut self, buffer: &[u8]) -> Result<()> {
            self.datagrams.lock().push(buffer.to_vec());
            *self.calls.lock() += 1;
            Ok(())
        }

        fn send_segmented(&mut self, buffer: &[u8], segment_size: usize) -> Result<bool> {
            if !self.segmented {
                return Ok(false);
            }
            let chunks = buffer.chunks(segment_size).collect::<Vec<_>>();
            for chunk in &chunks[..chunks.len() - 1] {
                assert_eq!(chunk.len(), segment_size);
            }
            assert!(buffer.len() <= 65_535);
            // Each datagram must be a whole shard: its declared length matches its size.
            for chunk in &chunks {
                let declared = 4 + u32::from_be_bytes(chunk[0..4].try_into().unwrap()) as usize;
                assert_eq!(declared, chunk.len());
            }
            self.datagrams
                .lock()
                .extend(chunks.iter().map(|chunk| chunk.to_vec()));
            *self.calls.lock() += 1;
            Ok(true)
        }
    }

    struct Datagrams(VecDeque<Vec<u8>>);

    impl SocketReader for Datagrams {
        fn recv(&mut self, buffer: &mut [u8]) -> ConResult<usize> {
            let Some(datagram) = self.0.pop_front() else {
                return alvr_common::try_again();
            };
            buffer[..datagram.len()].copy_from_slice(&datagram);
            Ok(datagram.len())
        }

        fn peek(&self, _: &mut [u8]) -> ConResult<usize> {
            unreachable!("datagram sockets are read in one call")
        }

        fn is_datagram(&self) -> bool {
            true
        }
    }

    const MAX_PACKET_SIZE: usize = 1400;

    fn socket(writer: Wire, reader: Datagrams) -> StreamSocket {
        socket_with_packet_size(writer, reader, MAX_PACKET_SIZE)
    }

    fn socket_with_packet_size(
        writer: Wire,
        reader: Datagrams,
        max_packet_size: usize,
    ) -> StreamSocket {
        StreamSocket {
            max_packet_size,
            send_socket: Arc::new(Mutex::new(Box::new(writer))),
            receive_socket: Box::new(reader),
            shard_recv_state: None,
            stream_recv_components: HashMap::new(),
            datagram_scratch: Vec::new(),
        }
    }

    // A frame like PyroWave's: many packets, most exactly filling a datagram, some shorter,
    // a few spanning several datagrams; then a big single packet like an HEVC frame.
    fn payload_sizes() -> Vec<usize> {
        payload_sizes_for(MAX_PACKET_SIZE)
    }

    fn payload_sizes_for(max_packet_size: usize) -> Vec<usize> {
        let header = bincode::serialized_size(&(0u64, 0u32, true)).unwrap() as usize;
        let full = max_packet_size - SHARD_PREFIX_SIZE - header;
        let mut sizes = vec![];
        for i in 0..500 {
            sizes.push(match i % 37 {
                0 => full - 1 - (i % 300),
                1 => full * 3 + 17,
                2 => full + 1,
                3 => 1,
                _ => full,
            });
        }
        sizes.push(300_000);
        sizes
    }

    fn send_frames(segmentation: bool, segmented_writer: bool) -> (Vec<Vec<u8>>, usize) {
        send_frames_with_packet_size(segmentation, segmented_writer, MAX_PACKET_SIZE)
    }

    fn send_frames_with_packet_size(
        segmentation: bool,
        segmented_writer: bool,
        max_packet_size: usize,
    ) -> (Vec<Vec<u8>>, usize) {
        USO_STATE.store(
            if segmentation { USO_WANTED } else { USO_OFF },
            std::sync::atomic::Ordering::Relaxed,
        );
        let datagrams = Arc::new(Mutex::new(vec![]));
        let calls = Arc::new(Mutex::new(0));
        let socket = socket_with_packet_size(
            Wire {
                datagrams: Arc::clone(&datagrams),
                calls: Arc::clone(&calls),
                segmented: segmented_writer,
            },
            Datagrams(VecDeque::new()),
            max_packet_size,
        );
        let mut sender = socket.request_stream::<(u64, u32, bool)>(3);

        let mut buffers = vec![];
        for (i, size) in payload_sizes_for(max_packet_size).into_iter().enumerate() {
            let mut buffer = sender
                .get_buffer(&(i as u64, size as u32, i % 2 == 0))
                .unwrap();
            for (j, byte) in buffer.get_range_mut(0, size).iter_mut().enumerate() {
                *byte = (i * 31 + j * 7) as u8;
            }
            buffers.push(buffer);
        }
        sender.send_many(buffers).unwrap();

        let datagrams = datagrams.lock().clone();
        let calls = *calls.lock();
        (datagrams, calls)
    }

    // Serialized: the tests share the process-wide segmentation state.
    #[test]
    fn segmented_send_and_single_call_receive() {
        let (plain, plain_calls) = send_frames(false, false);
        let (segmented, segmented_calls) = send_frames(true, true);
        let (refused, refused_calls) = send_frames(true, false);
        // Datagrams too big for two per call (a 32 KB packet size): sent as they are, no staging,
        // and still the same datagrams.
        let (big_plain, _) = send_frames_with_packet_size(false, false, 32_768);
        let (big_segmented, big_segmented_calls) = send_frames_with_packet_size(true, true, 32_768);
        USO_STATE.store(USO_OFF, std::sync::atomic::Ordering::Relaxed);
        assert_eq!(big_plain, big_segmented);
        assert_eq!(big_segmented_calls, big_segmented.len());

        // Same datagrams on the wire whichever way they were handed over.
        assert_eq!(plain, segmented);
        assert_eq!(plain, refused);
        assert_eq!(plain_calls, plain.len());
        assert_eq!(refused_calls, refused.len());
        assert!(
            segmented_calls * 10 < plain_calls,
            "{segmented_calls} calls vs {plain_calls}"
        );

        // And the receive side reassembles every packet from them.
        let mut socket = socket(
            Wire {
                datagrams: Arc::new(Mutex::new(vec![])),
                calls: Arc::new(Mutex::new(0)),
                segmented: false,
            },
            Datagrams(segmented.into_iter().collect()),
        );
        let sizes = payload_sizes();
        let mut receiver = socket.subscribe_to_stream::<(u64, u32, bool)>(3, sizes.len() + 1);
        for _ in 0..plain.len() {
            assert!(socket.recv().is_ok());
        }
        assert!(matches!(socket.recv(), Err(ConnectionError::TryAgain(_))));

        for (i, size) in sizes.iter().enumerate() {
            let Ok(data) = receiver.recv(Duration::from_millis(10)) else {
                panic!("packet {i} was not reassembled");
            };
            assert!(!data.had_packet_loss());
            let (header, payload) = data.get().unwrap();
            assert_eq!(header, (i as u64, *size as u32, i % 2 == 0));
            assert_eq!(payload.len(), *size);
            assert!(payload
                .iter()
                .enumerate()
                .all(|(j, byte)| *byte == (i * 31 + j * 7) as u8));
        }
    }
}
