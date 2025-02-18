/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/**
 * @packageDocumentation
 * @module mqtt
 */

import * as mqtt from "mqtt";
import * as WebsocketUtils from "./ws";
import * as auth from "./auth";
import { Trie, TrieOp, Node as TrieNode } from "./trie";

import { BufferedEventEmitter } from "../common/event";
import { CrtError } from "../browser";
import { ClientBootstrap, SocketOptions } from "./io";
import {
    QoS,
    Payload,
    MqttRequest,
    MqttSubscribeRequest,
    MqttWill,
    OnMessageCallback,
    MqttConnectionConnected,
    MqttConnectionDisconnected,
    MqttConnectionError,
    MqttConnectionInterrupted,
    MqttConnectionResumed
} from "../common/mqtt";
export { QoS, Payload, MqttRequest, MqttSubscribeRequest, MqttWill } from "../common/mqtt";

/**
 * @category MQTT
 */
export type WebsocketOptions = WebsocketUtils.WebsocketOptions;

/**
 * @category MQTT
 */
export type AWSCredentials = auth.AWSCredentials;

/**
 * Configuration options for an MQTT connection
 *
 * @category MQTT
 */
export interface MqttConnectionConfig {
    /**
    * ID to place in CONNECT packet. Must be unique across all devices/clients.
    * If an ID is already in use, the other client will be disconnected.
    */
    client_id: string;

    /** Server name to connect to */
    host_name: string;

    /** Server port to connect to */
    port: number;

    /** Socket options, ignored in browser */
    socket_options: SocketOptions;

    /**
     * Whether or not to start a clean session with each reconnect.
     * If True, the server will forget all subscriptions with each reconnect.
     * Set False to request that the server resume an existing session
     * or start a new session that may be resumed after a connection loss.
     * The `session_present` bool in the connection callback informs
     * whether an existing session was successfully resumed.
     * If an existing session is resumed, the server remembers previous subscriptions
     * and sends mesages (with QoS1 or higher) that were published while the client was offline.
     */
    clean_session?: boolean;

    /**
     * The keep alive value, in seconds, to send in CONNECT packet.
     * A PING will automatically be sent at this interval.
     * The server will assume the connection is lost if no PING is received after 1.5X this value.
     * This duration must be longer than {@link ping_timeout}.
     */
    keep_alive?: number;

    /**
     * Milliseconds to wait for ping response before client assumes
     * the connection is invalid and attempts to reconnect.
     * This duration must be shorter than keep_alive_secs.
     * Alternatively, TCP keep-alive via :attr:`SocketOptions.keep_alive`
     * may accomplish this in a more efficient (low-power) scenario,
     * but keep-alive options may not work the same way on every platform and OS version.
     */
    ping_timeout?: number;

    /**
     * Milliseconds to wait for the response to the operation requires response by protocol.
     * Set to zero to disable timeout. Otherwise, the operation will fail if no response is
     * received within this amount of time after the packet is written to the socket.
     * It applied to PUBLISH (QoS>0) and UNSUBSCRIBE now.
     */
    protocol_operation_timeout?: number;

    /**
     * Will to send with CONNECT packet. The will is
     * published by the server when its connection to the client is unexpectedly lost.
     */
    will?: MqttWill;

    /** Username to connect with */
    username?: string;

    /** Password to connect with */
    password?: string;

    /** Options for the underlying websocket connection */
    websocket?: WebsocketOptions;

    /** AWS credentials, which will be used to sign the websocket request */
    credentials?: AWSCredentials;
}

/**
 * MQTT client
 *
 * @category MQTT
 */
export class MqttClient {
    constructor(bootstrap?: ClientBootstrap) {

    }

    /**
     * Creates a new {@link MqttClientConnection}
     * @param config Configuration for the connection
     * @returns A new connection
     */
    new_connection(config: MqttConnectionConfig) {
        return new MqttClientConnection(this, config);
    }
}

/** @internal */
class TopicTrie extends Trie<OnMessageCallback | undefined> {
    constructor() {
        super('/');
    }

    protected find_node(key: string, op: TrieOp) {
        const parts = this.split_key(key);
        let current = this.root;
        let parent = undefined;
        for (const part of parts) {
            let child = current.children.get(part);
            if (!child) {
                child = current.children.get('#');
                if (child) {
                    return child;
                }

                child = current.children.get('+');
            }
            if (!child) {
                if (op == TrieOp.Insert) {
                    current.children.set(part, child = new TrieNode(part));
                }
                else {
                    return undefined;
                }
            }
            parent = current;
            current = child;
        }
        if (parent && op == TrieOp.Delete) {
            parent.children.delete(current.key!);
        }
        return current;
    }
}

/**
 * Converts payload to Buffer or string regardless of the supplied type
 * @param payload The payload to convert
 * @internal
 */
function normalize_payload(payload: Payload): Buffer | string {
    if (payload instanceof Buffer) {
        // pass Buffer through
        return payload;
    }
    if (typeof payload === 'string') {
        // pass string through
        return payload;
    }
    if (ArrayBuffer.isView(payload)) {
        // return Buffer with view upon the same bytes (no copy)
        const view = payload as ArrayBufferView;
        return Buffer.from(view.buffer, view.byteOffset, view.byteLength);
    }
    if (payload instanceof ArrayBuffer) {
        // return Buffer with view upon the same bytes (no copy)
        return Buffer.from(payload);
    }
    if (typeof payload === 'object') {
        // Convert Object to JSON string
        return JSON.stringify(payload);
    }
    throw new TypeError("payload parameter must be a string, object, or DataView.");
}

/**
 * MQTT client connection
 *
 * @category MQTT
 */
export class MqttClientConnection extends BufferedEventEmitter {
    private connection: mqtt.MqttClient;
    private subscriptions = new TopicTrie();
    private connection_count = 0;

    /**
     * @param client The client that owns this connection
     * @param config The configuration for this connection
     */
    constructor(
        readonly client: MqttClient,
        private config: MqttConnectionConfig) {
        super();

        const create_websocket_stream = (client: mqtt.MqttClient) => WebsocketUtils.create_websocket_stream(this.config);
        const transform_websocket_url = (url: string, options: mqtt.IClientOptions, client: mqtt.MqttClient) => WebsocketUtils.create_websocket_url(this.config);

        const will = this.config.will ? {
            topic: this.config.will.topic,
            payload: normalize_payload(this.config.will.payload),
            qos: this.config.will.qos,
            retain: this.config.will.retain,
        } : undefined;

        const websocketXform = (config.websocket || {}).protocol != 'wss-custom-auth' ? transform_websocket_url : undefined;

        this.connection = new mqtt.MqttClient(
            create_websocket_stream,
            {
                // service default is 1200 seconds
                keepalive: this.config.keep_alive ? this.config.keep_alive : 1200,
                clientId: this.config.client_id,
                connectTimeout: this.config.ping_timeout ? this.config.ping_timeout : 30 * 1000,
                clean: this.config.clean_session,
                username: this.config.username,
                password: this.config.password,
                reconnectPeriod: 0,
                will: will,
                transformWsUrl: websocketXform,
            }
        );

        this.connection.on('connect', this.on_connect);
        this.connection.on('error', this.on_error);
        this.connection.on('message', this.on_message);
        this.connection.on('offline', this.on_offline);
        this.connection.on('end', this.on_disconnected);
    }

    /**
     * Emitted when the connection successfully establishes itself for the first time
     *
     * @param event the type of event (connect)
     * @param listener the event listener to use
     *
     * @event
     */
    on(event: 'connect', listener: MqttConnectionConnected): this;

    /**
     * Emitted when connection has disconnected sucessfully.
     *
     * @param event the type of event (disconnect)
     * @param listener the event listener to use
     *
     * @event
     */
    on(event: 'disconnect', listener: MqttConnectionDisconnected): this;

    /**
     * Emitted when an error occurs.  The error will contain the error
     * code and message.
     *
     * @param event the type of event (error)
     * @param listener the event listener to use
     *
     * @event
     */
    on(event: 'error', listener: MqttConnectionError): this;

    /**
     * Emitted when the connection is dropped unexpectedly. The error will contain the error
     * code and message.  The underlying mqtt implementation will attempt to reconnect.
     *
     * @param event the type of event (interrupt)
     * @param listener the event listener to use
     *
     * @event
     */
    on(event: 'interrupt', listener: MqttConnectionInterrupted): this;

    /**
     * Emitted when the connection reconnects (after an interrupt). Only triggers on connections after the initial one.
     *
     * @param event the type of event (resume)
     * @param listener the event listener to use
     *
     * @event
     */
    on(event: 'resume', listener: MqttConnectionResumed): this;

    /**
     * Emitted when any MQTT publish message arrives.
     *
     * @param event the type of event (message)
     * @param listener the event listener to use
     *
     * @event
     */
    on(event: 'message', listener: OnMessageCallback): this;

    on(event: string | symbol, listener: (...args: any[]) => void): this {
        return super.on(event, listener);
    }

    private on_connect = (connack: mqtt.IConnackPacket) => {
        this.on_online(connack.sessionPresent);
    }

    private on_online = (session_present: boolean) => {
        if (++this.connection_count == 1) {
            this.emit('connect', session_present);
        } else {
            this.emit('resume', 0, session_present);
        }
    }

    private on_offline = () => {
        this.emit('interrupt', -1);
    }

    private on_disconnected = () => {
        this.emit('disconnect');
    }

    private on_error = (error: Error) => {
        this.emit('error', new CrtError(error))
    }

    private on_message = (topic: string, payload: Buffer, packet: mqtt.IPublishPacket) => {
        // pass payload as ArrayBuffer
        const array_buffer = payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength)

        const callback = this.subscriptions.find(topic);
        if (callback) {
            callback(topic, array_buffer, packet.dup, packet.qos, packet.retain);
        }
        this.emit('message', topic, array_buffer, packet.dup, packet.qos, packet.retain);
    }

    /**
     * Open the actual connection to the server (async).
     * @returns A Promise which completes whether the connection succeeds or fails.
     *          If connection fails, the Promise will reject with an exception.
     *          If connection succeeds, the Promise will return a boolean that is
     *          true for resuming an existing session, or false if the session is new
     */
    async connect() {
        setTimeout(() => { this.uncork() }, 0);
        return new Promise<boolean>((resolve, reject) => {
            const on_connect_error = (error: Error) => {
                reject(new CrtError(error));
            };
            this.connection.once('connect', (connack: mqtt.IConnackPacket) => {
                this.connection.removeListener('error', on_connect_error);
                resolve(connack.sessionPresent);
            });
            this.connection.once('error', on_connect_error);
        });
    }

    /**
     * The connection will automatically reconnect. To cease reconnection attempts, call {@link disconnect}.
     * To resume the connection, call {@link connect}.
     * @deprecated
     */
    async reconnect() {
        return this.connect();
    }

    /**
     * Publish message (async).
     * If the device is offline, the PUBLISH packet will be sent once the connection resumes.
     *
     * @param topic Topic name
     * @param payload Contents of message
     * @param qos Quality of Service for delivering this message
     * @param retain If true, the server will store the message and its QoS so that it can be
     *               delivered to future subscribers whose subscriptions match the topic name
     * @returns Promise which returns a {@link MqttRequest} which will contain the packet id of
     *          the PUBLISH packet.
     *
     * * For QoS 0, completes as soon as the packet is sent.
     * * For QoS 1, completes when PUBACK is received.
     * * For QoS 2, completes when PUBCOMP is received.
     */
    async publish(topic: string, payload: Payload, qos: QoS, retain: boolean = false): Promise<MqttRequest> {
        let payload_data = normalize_payload(payload);
        return new Promise((resolve, reject) => {
            this.connection.publish(topic, payload_data, { qos: qos, retain: retain }, (error, packet) => {
                if (error) {
                    reject(new CrtError(error));
                    return this.on_error(error);
                }
                resolve({ packet_id: (packet as mqtt.IPublishPacket).messageId })
            });
        });
    }

    /**
     * Subscribe to a topic filter (async).
     * The client sends a SUBSCRIBE packet and the server responds with a SUBACK.
     *
     * subscribe() may be called while the device is offline, though the async
     * operation cannot complete successfully until the connection resumes.
     *
     * Once subscribed, `callback` is invoked each time a message matching
     * the `topic` is received. It is possible for such messages to arrive before
     * the SUBACK is received.
     *
     * @param topic Subscribe to this topic filter, which may include wildcards
     * @param qos Maximum requested QoS that server may use when sending messages to the client.
     *            The server may grant a lower QoS in the SUBACK
     * @param on_message Optional callback invoked when message received.
     * @returns Promise which returns a {@link MqttSubscribeRequest} which will contain the
     *          result of the SUBSCRIBE. The Promise resolves when a SUBACK is returned
     *          from the server or is rejected when an exception occurs.
     */
    async subscribe(topic: string, qos: QoS, on_message?: OnMessageCallback): Promise<MqttSubscribeRequest> {
        this.subscriptions.insert(topic, on_message);
        return new Promise((resolve, reject) => {
            this.connection.subscribe(topic, { qos: qos }, (error, packet) => {
                if (error) {
                    reject(new CrtError(error))
                    return this.on_error(error);
                }
                const sub = (packet as mqtt.ISubscriptionGrant[])[0];
                resolve({ topic: sub.topic, qos: sub.qos });
            });
        });
    }

    /**
    * Unsubscribe from a topic filter (async).
    * The client sends an UNSUBSCRIBE packet, and the server responds with an UNSUBACK.
    * @param topic The topic filter to unsubscribe from. May contain wildcards.
    * @returns Promise wihch returns a {@link MqttRequest} which will contain the packet id
    *          of the UNSUBSCRIBE packet being acknowledged. Promise is resolved when an
    *          UNSUBACK is received from the server or is rejected when an exception occurs.
    */
    async unsubscribe(topic: string): Promise<MqttRequest> {
        this.subscriptions.remove(topic);
        return new Promise((resolve, reject) => {
            this.connection.unsubscribe(topic, undefined, (error?: Error, packet?: mqtt.Packet) => {
                if (error) {
                    reject(new CrtError(error));
                    return this.on_error(error);
                }
                resolve({
                    packet_id: packet
                        ? (packet as mqtt.IUnsubackPacket).messageId
                        : undefined,
                });
            });

        });
    }

    /**
     * Close the connection (async).
     * @returns Promise which completes when the connection is closed.
    */
    async disconnect() {
        return new Promise((resolve) => {
            this.connection.end(undefined, resolve)
        });
    }
}
