mod platform;

use kitvr_core::proto::control::{decode_message, encode_message, Message};
use kitvr_core::proto::{DecodeError, CONTROL_PORT, PROTOCOL_VERSION};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};

fn host_name() -> String {
    std::env::var("COMPUTERNAME")
        .or_else(|_| std::env::var("HOSTNAME"))
        .unwrap_or_else(|_| "kitvr-server".to_string())
}

fn handle(mut s: TcpStream) -> std::io::Result<()> {
    let mut buf: Vec<u8> = Vec::new();
    let mut tmp = [0u8; 1024];
    loop {
        let n = s.read(&mut tmp)?;
        if n == 0 {
            return Ok(());
        }
        buf.extend_from_slice(&tmp[..n]);
        loop {
            match decode_message(&buf) {
                Ok((msg, used)) => {
                    buf.drain(..used);
                    println!("<- {msg:?}");
                    match msg {
                        Message::Hello { .. } => {
                            let reply = Message::Hello { version: PROTOCOL_VERSION, name: host_name() };
                            s.write_all(&encode_message(&reply))?;
                        }
                        Message::Ping { ts_us } => {
                            s.write_all(&encode_message(&Message::Pong { ts_us }))?;
                        }
                        Message::Bye => return Ok(()),
                        _ => {}
                    }
                }
                Err(DecodeError::Truncated) => break,
                Err(e) => {
                    eprintln!("protocol error: {e}");
                    return Ok(());
                }
            }
        }
    }
}

fn main() -> std::io::Result<()> {
    println!("kitvr-server, platform backend: {}", platform::NAME);
    println!("capture: {:?}", platform::create_capture().err().map(|e| e.to_string()));
    println!("encoder: {:?}", platform::create_encoder().err().map(|e| e.to_string()));
    println!("input:   {:?}", platform::create_input().err().map(|e| e.to_string()));

    let listener = TcpListener::bind(("0.0.0.0", CONTROL_PORT))?;
    println!("control channel listening on :{CONTROL_PORT}");
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                if let Err(e) = handle(s) {
                    eprintln!("connection error: {e}");
                }
            }
            Err(e) => eprintln!("accept error: {e}"),
        }
    }
    Ok(())
}
