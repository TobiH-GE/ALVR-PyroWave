use crate::LOCAL_IP;

use super::{SocketReader, SocketWriter};
use alvr_common::{anyhow::Result, ConResult, HandleTryAgain};
use alvr_session::{DscpTos, SocketBufferSize};
use socket2::{MaybeUninitSlice, Socket};
use std::{
    ffi::c_int,
    mem::MaybeUninit,
    net::{IpAddr, UdpSocket},
    ptr,
    time::Duration,
};

// Create tokio socket, convert to socket2, apply settings, convert back to tokio. This is done to
// let tokio set all the internal parameters it needs from the start.
pub fn bind(
    port: u16,
    dscp: Option<DscpTos>,
    send_buffer_bytes: SocketBufferSize,
    recv_buffer_bytes: SocketBufferSize,
) -> Result<UdpSocket> {
    let socket = UdpSocket::bind((LOCAL_IP, port))?.into();

    crate::set_socket_buffers(&socket, send_buffer_bytes, recv_buffer_bytes).ok();

    crate::set_dscp(&socket, dscp);

    Ok(socket.into())
}

pub fn connect(
    socket: &UdpSocket,
    peer_ip: IpAddr,
    port: u16,
    timeout: Duration,
) -> Result<(UdpSocket, Socket)> {
    socket.connect((peer_ip, port))?;
    socket.set_read_timeout(Some(timeout))?;

    Ok((socket.try_clone()?, socket.try_clone()?.into()))
}

impl SocketWriter for UdpSocket {
    fn send(&mut self, buffer: &[u8]) -> Result<()> {
        UdpSocket::send(self, buffer)?;

        Ok(())
    }

    // UDP segmentation offload (UDP_SEND_MSG_SIZE, Windows 10 2004 and later): the stack, or the
    // network adapter, cuts the buffer into datagrams of segment_size. Taken over from the JPEG XS
    // streamer, where one call per datagram capped the send rate at 0.4-2.7 Gbit/s.
    #[cfg(windows)]
    fn send_segmented(&mut self, buffer: &[u8], segment_size: usize) -> Result<bool> {
        use crate::{USO_OFF, USO_ON, USO_SEGMENT, USO_STATE, USO_UNSUPPORTED};
        use std::{os::windows::io::AsRawSocket, sync::atomic::Ordering};

        #[link(name = "ws2_32")]
        extern "system" {
            fn setsockopt(s: usize, level: i32, name: i32, value: *const u8, len: i32) -> i32;
        }
        const IPPROTO_UDP: i32 = 17;
        const UDP_SEND_MSG_SIZE: i32 = 2;

        match USO_STATE.load(Ordering::Relaxed) {
            USO_OFF | USO_UNSUPPORTED => return Ok(false),
            USO_ON if USO_SEGMENT.load(Ordering::Relaxed) == segment_size => (),
            _ => {
                // First use, or a different segment size: set it on the socket. It stays set,
                // and a send no larger than one segment still goes out as a single datagram.
                let size = segment_size as u32;
                let rc = unsafe {
                    setsockopt(
                        self.as_raw_socket() as usize,
                        IPPROTO_UDP,
                        UDP_SEND_MSG_SIZE,
                        &size as *const u32 as *const u8,
                        4,
                    )
                };
                if rc != 0 {
                    USO_STATE.store(USO_UNSUPPORTED, Ordering::Relaxed);
                    return Ok(false);
                }
                USO_SEGMENT.store(segment_size, Ordering::Relaxed);
                USO_STATE.store(USO_ON, Ordering::Relaxed);
            }
        }

        match UdpSocket::send(self, buffer) {
            Ok(_) => Ok(true),
            // The stack took the option but refuses the send (e.g. WSAEMSGSIZE, too old a
            // Windows): nothing went out, so the caller can still send these one by one.
            Err(_) => {
                USO_STATE.store(USO_UNSUPPORTED, Ordering::Relaxed);
                Ok(false)
            }
        }
    }
}

impl SocketReader for Socket {
    fn is_datagram(&self) -> bool {
        true
    }

    fn recv(&mut self, buffer: &mut [u8]) -> ConResult<usize> {
        Socket::recv(self, unsafe {
            &mut *(ptr::from_mut(buffer) as *mut [MaybeUninit<u8>])
        })
        .handle_try_again()
    }

    fn peek(&self, buffer: &mut [u8]) -> ConResult<usize> {
        #[cfg(windows)]
        const FLAGS: c_int = 0x02 | 0x8000; // MSG_PEEK | MSG_PARTIAL
        #[cfg(not(windows))]
        const FLAGS: c_int = 0x02 | 0x20; // MSG_PEEK | MSG_TRUNC

        let buffer = MaybeUninitSlice::new(unsafe {
            &mut *(ptr::from_mut(buffer) as *mut [MaybeUninit<u8>])
        });
        Ok(self
            .recv_vectored_with_flags(&mut [buffer], FLAGS)
            .handle_try_again()?
            .0)
    }
}
