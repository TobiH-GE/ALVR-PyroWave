mod backend;
mod control_socket;
mod stream_socket;

use alvr_common::{anyhow::Result, info};
use alvr_session::{DscpTos, SocketBufferSize};
use socket2::Socket;
use std::{
    net::{IpAddr, Ipv4Addr},
    time::Duration,
};

pub use control_socket::*;
pub use stream_socket::*;

pub const LOCAL_IP: IpAddr = IpAddr::V4(Ipv4Addr::UNSPECIFIED);
pub const CONTROL_PORT: u16 = 9943;
pub const HANDSHAKE_PACKET_SIZE_BYTES: usize = 56; // this may change in future protocols
pub const KEEPALIVE_INTERVAL: Duration = Duration::from_millis(500);
pub const KEEPALIVE_TIMEOUT: Duration = Duration::from_secs(2);

pub const MDNS_SERVICE_TYPE: &str = "_alvr._tcp.local.";
pub const MDNS_PROTOCOL_KEY: &str = "protocol";
pub const MDNS_DEVICE_ID_KEY: &str = "device_id";

pub const WIRED_CLIENT_HOSTNAME: &str = "client.wired";

// UDP segmentation offload for the stream sender (Windows): one send call hands the stack many
// datagrams, which it cuts apart itself. Process-wide on purpose -- there is one stream socket,
// and the setting is fixed for a session. See StreamSender::send_many.
use std::sync::atomic::{AtomicU8, AtomicUsize, Ordering};
#[cfg_attr(not(windows), allow(dead_code))]
const USO_OFF: u8 = 0;
const USO_WANTED: u8 = 1;
#[cfg_attr(not(windows), allow(dead_code))]
const USO_ON: u8 = 2;
#[cfg_attr(not(windows), allow(dead_code))]
const USO_UNSUPPORTED: u8 = 3;
static USO_STATE: AtomicU8 = AtomicU8::new(USO_OFF);
#[cfg_attr(not(windows), allow(dead_code))]
static USO_SEGMENT: AtomicUsize = AtomicUsize::new(0);

// Only has an effect on Windows, and only for a UDP stream socket.
pub fn set_udp_send_segmentation(enabled: bool) {
    USO_STATE.store(
        if enabled && cfg!(windows) {
            USO_WANTED
        } else {
            USO_OFF
        },
        Ordering::Relaxed,
    );
    USO_SEGMENT.store(0, Ordering::Relaxed);
}

fn udp_send_segmentation_wanted() -> bool {
    matches!(USO_STATE.load(Ordering::Relaxed), USO_WANTED | USO_ON)
}

pub fn udp_segmentation_state() -> &'static str {
    match USO_STATE.load(Ordering::Relaxed) {
        USO_OFF => "off",
        USO_WANTED => "on, not used yet",
        USO_ON => "on",
        _ => "refused by Windows, one call per datagram",
    }
}

fn set_socket_buffers(
    socket: &socket2::Socket,
    send_buffer_bytes: SocketBufferSize,
    recv_buffer_bytes: SocketBufferSize,
) -> Result<()> {
    info!(
        "Initial socket buffer size: send: {}B, recv: {}B",
        socket.send_buffer_size()?,
        socket.recv_buffer_size()?
    );

    {
        let maybe_size = match send_buffer_bytes {
            SocketBufferSize::Default => None,
            SocketBufferSize::Maximum => Some(u32::MAX),
            SocketBufferSize::Custom(size) => Some(size),
        };

        if let Some(size) = maybe_size {
            if let Err(e) = socket.set_send_buffer_size(size as usize) {
                info!("Error setting socket send buffer: {e}");
            } else {
                info!(
                    "Set socket send buffer succeeded: {}",
                    socket.send_buffer_size()?
                );
            }
        }
    }

    {
        let maybe_size = match recv_buffer_bytes {
            SocketBufferSize::Default => None,
            SocketBufferSize::Maximum => Some(u32::MAX),
            SocketBufferSize::Custom(size) => Some(size),
        };

        if let Some(size) = maybe_size {
            if let Err(e) = socket.set_recv_buffer_size(size as usize) {
                info!("Error setting socket recv buffer: {e}");
            } else {
                info!(
                    "Set socket recv buffer succeeded: {}",
                    socket.recv_buffer_size()?
                );
            }
        }
    }

    Ok(())
}

fn set_dscp(socket: &Socket, dscp: Option<DscpTos>) {
    // https://en.wikipedia.org/wiki/Differentiated_services
    if let Some(dscp) = dscp {
        let tos = match dscp {
            DscpTos::BestEffort => 0,
            DscpTos::ClassSelector(precedence) => precedence << 3,
            DscpTos::AssuredForwarding {
                class,
                drop_probability,
            } => (class << 3) | drop_probability as u8,
            DscpTos::ExpeditedForwarding => 0b101110,
        };

        socket.set_tos((tos << 2) as u32).ok();
    }
}
