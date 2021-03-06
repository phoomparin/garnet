// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![deny(warnings)]

extern crate failure;
extern crate fidl;
extern crate fidl_fuchsia_bluetooth as fidl_bt;
extern crate fidl_fuchsia_bluetooth_le as fidl_ble;
extern crate fuchsia_app as app;
extern crate fuchsia_async as async;
extern crate fuchsia_bluetooth as bt;
extern crate fuchsia_zircon as zx;
extern crate futures;
extern crate parking_lot;
extern crate serde;
extern crate serde_json;

#[macro_use]
extern crate fuchsia_syslog as syslog;

#[macro_use]
extern crate rouille;

#[macro_use]
extern crate serde_derive;

use bt::error::Error as BTError;
use failure::Error;
use futures::channel::mpsc;
use parking_lot::{Mutex, RwLock};
use rouille::{Request, Response};
use serde_json::{to_value, Value};
use std::collections::HashMap;
use std::io::Read;
use std::sync::Arc;
use std::thread;

mod common;

use common::bluetooth_facade::BluetoothFacade;
use common::sl4f_executor::run_fidl_loop;
use common::sl4f_types::{AsyncRequest, AsyncResponse, ClientData, CommandRequest, CommandResponse};

// Config, flexible for any ip/port combination
const SERVER_IP: &str = "0.0.0.0";
const SERVER_PORT: &str = "80";

// HTTP Server using Rouille
fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["sl4f"]).expect("Can't init logger");
    fx_log_info!("Starting sl4f server");

    let address = format!("{}:{}", SERVER_IP, SERVER_PORT);
    fx_log_info!("Now listening on: {:?}", address);

    // Create empty hash table + bt facade for storing state
    let clients: Arc<Mutex<HashMap<String, ClientData>>> = Arc::new(Mutex::new(HashMap::new()));
    // TODO(aniramakri): Add support for other connectivity stacks
    let bt_facade: Arc<RwLock<BluetoothFacade>> = BluetoothFacade::new(None, None);

    // Create channel for communication: rouille sync side -> async exec side
    let (rouille_sender, async_receiver) = mpsc::unbounded();
    let bt_facade_async = bt_facade.clone();

    // Create the async execution thread
    thread::spawn(move || run_fidl_loop(bt_facade_async, async_receiver));
    // Start listening on address
    rouille::start_server(address, move |request| {
        serve(
            &request,
            clients.clone(),
            bt_facade.clone(),
            rouille_sender.clone(),
        )
    });
}

// Handles all incoming requests to SL4F server, routes accordingly
fn serve(
    request: &Request, clients: Arc<Mutex<HashMap<String, ClientData>>>,
    bt_facade: Arc<RwLock<BluetoothFacade>>, rouille_sender: mpsc::UnboundedSender<AsyncRequest>,
) -> Response {
    router!(request,
            (GET) (/) => {
                // Parse the command request
                fx_log_info!(tag: "serve", "Received command request.");
                client_request(&request,  bt_facade.clone(), rouille_sender.clone())
            },
            (GET) (/init) => {
                // Initialize a client
                fx_log_info!(tag: "serve", "Received init request.");
                client_init(&request, clients.clone())
            },
            (GET) (/print_clients) => {
                // Print information about all clients
                fx_log_info!(tag: "serve", "Received print client request.");
                const PRINT_ACK: &str = "Successfully printed clients.";
                rouille::Response::json(&PRINT_ACK)
            },
            (GET) (/cleanup) => {
                fx_log_info!(tag: "serve", "Received server cleanup request.");
                server_cleanup(&request, bt_facade.clone())
            },
            _ => {
                fx_log_err!(tag: "serve", "Received unknown server request.");
                const FAIL_REQUEST_ACK: &str = "Unknown GET request.";
                let res = CommandResponse::new(0, None, serde::export::Some(FAIL_REQUEST_ACK.to_string()));
                rouille::Response::json(&res)
            }
        )
}

// Given the request, map the test request to a FIDL query and execute
// asynchronously
fn client_request(
    request: &Request, _bt_facade: Arc<RwLock<BluetoothFacade>>,
    rouille_sender: mpsc::UnboundedSender<AsyncRequest>,
) -> Response {
    const FAIL_TEST_ACK: &str = "Command failed";

    let (method_id, method_name, method_params) = match parse_request(request) {
        Ok(res) => res,
        Err(e) => {
            fx_log_err!(tag: "client_request", "Failed to parse request. {:?}", e);
            return Response::json(&FAIL_TEST_ACK);
        }
    };

    // Create channel for async thread to respond to
    // Package response and ship over JSON RPC
    let (async_sender, rouille_receiver) = std::sync::mpsc::channel();
    let req = AsyncRequest::new(async_sender, method_id, method_name, method_params);
    rouille_sender
        .unbounded_send(req)
        .expect("Failed to send request to async thread.");
    let resp: AsyncResponse = rouille_receiver.recv().unwrap();

    fx_log_info!(tag: "client_request", "Received async thread response: {:?}", resp);
    _bt_facade.read().print_facade();

    match resp.res {
        Ok(r) => {
            let res = CommandResponse::new(method_id, Some(r), None);
            rouille::Response::json(&res)
        }
        Err(e) => {
            let res = CommandResponse::new(method_id, None, serde::export::Some(e.to_string()));
            rouille::Response::json(&res)
        }
    }
}

// Initializes a new client, adds to clients, a thread-safe HashMap
// Returns a rouille::Response
fn client_init(request: &Request, clients: Arc<Mutex<HashMap<String, ClientData>>>) -> Response {
    const INIT_ACK: &str = "Recieved init request.";
    const FAIL_INIT_ACK: &str = "Failed to init client.";

    let (_, _, method_params) = match parse_request(request) {
        Ok(res) => res,
        Err(_) => return Response::json(&FAIL_INIT_ACK),
    };

    let client_id_raw = match method_params.get("client_id") {
        Some(id) => Some(id).unwrap().clone(),
        None => return Response::json(&FAIL_INIT_ACK),
    };

    let client_id = client_id_raw.as_str().map(String::from).unwrap();
    let client_data = ClientData {
        client_id: client_id.clone(),
    };

    if clients.lock().contains_key(&client_id) {
        fx_log_warn!(tag: "client_init",
            "Key: {:?} already exists in clients. ",
            &client_id
        );
        return rouille::Response::json(&FAIL_INIT_ACK);
    }

    clients.lock().insert(client_id, client_data);
    fx_log_info!(tag: "client_init", "Updated clients: {:?}", clients);

    rouille::Response::json(&INIT_ACK)
}

// Given a request, grabs the method id, name, and parameters
// Return BTError if fail
fn parse_request(request: &Request) -> Result<(u32, String, Value), Error> {
    let mut data = match request.data() {
        Some(d) => d,
        None => return Err(BTError::new("Failed to parse request buffer.").into()),
    };

    let mut buf: String = String::new();
    if data.read_to_string(&mut buf).is_err() {
        return Err(BTError::new("Failed to read request buffer.").into());
    }

    let request_data: CommandRequest = match serde_json::from_str(&buf) {
        Ok(tdata) => tdata,
        Err(_) => return Err(BTError::new("Failed to unpack request data.").into()),
    };

    let method_id = request_data.id.clone();
    let method_name = request_data.method.clone();
    let method_params = request_data.params.clone();
    fx_log_info!(tag: "parse_request",
        "method id: {:?}, name: {:?}, args: {:?}",
        method_id, method_name, method_params
    );

    Ok((method_id, method_name, method_params))
}

fn server_cleanup(request: &Request, bt_facade: Arc<RwLock<BluetoothFacade>>) -> Response {
    const FAIL_CLEANUP_ACK: &str = "Failed to cleanup SL4F resources.";
    const CLEANUP_ACK: &str = "Successful cleanup of SL4F resources.";

    fx_log_info!(tag: "server_cleanup", "Cleaning up server state");
    let (method_id, _, _) = match parse_request(request) {
        Ok(res) => res,
        Err(_) => return Response::json(&FAIL_CLEANUP_ACK),
    };

    // Verify facade is cleaned up
    bt_facade.write().cleanup_facade();
    bt_facade.read().print_facade();

    let ack = match to_value(serde::export::Some(CLEANUP_ACK.to_string())) {
        Ok(v) => CommandResponse::new(method_id, Some(v), None),
        Err(e) => CommandResponse::new(method_id, None, Some(e.to_string())),
    };

    rouille::Response::json(&ack)
}
