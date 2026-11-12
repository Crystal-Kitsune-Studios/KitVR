use std::io;
use std::os::fd::OwnedFd;

use ashpd::desktop::{
    screencast::{CursorMode, Screencast, SelectSourcesOptions, SourceType},
    Session,
};

pub struct WaylandCapture {
    runtime: tokio::runtime::Runtime,
    portal: Screencast,
    session: Session<Screencast>,
    node: u32,
}

impl std::fmt::Debug for WaylandCapture {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("WaylandCapture").field("node", &self.node).finish()
    }
}

fn portal_error(error: ashpd::Error) -> io::Error {
    io::Error::other(format!("Wayland screen sharing failed: {error}"))
}

impl WaylandCapture {
    pub fn new() -> io::Result<Self> {
        let runtime = tokio::runtime::Runtime::new()?;
        println!("Select a screen or window in the Wayland screen-sharing dialog.");
        let (portal, session, node) = runtime.block_on(async {
            let portal = Screencast::new().await.map_err(portal_error)?;
            let session = portal.create_session(Default::default()).await.map_err(portal_error)?;
            portal.select_sources(
                &session,
                SelectSourcesOptions::default()
                    .set_sources(SourceType::MONITOR | SourceType::WINDOW)
                    .set_cursor_mode(CursorMode::EMBEDDED)
                    .set_multiple(false),
            ).await.map_err(portal_error)?.response().map_err(portal_error)?;
            let response = portal.start(&session, None, Default::default())
                .await.map_err(portal_error)?.response().map_err(portal_error)?;
            let node = response.streams().first()
                .ok_or_else(|| io::Error::other("Wayland screen sharing returned no stream"))?
                .pipe_wire_node_id();
            Ok::<_, io::Error>((portal, session, node))
        })?;
        Ok(Self { runtime, portal, session, node })
    }

    pub fn open_remote(&self) -> io::Result<OwnedFd> {
        self.runtime.block_on(self.portal.open_pipe_wire_remote(&self.session, Default::default()))
            .map_err(portal_error)
    }

    pub fn node(&self) -> u32 {
        self.node
    }
}
