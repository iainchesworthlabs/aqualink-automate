// Minimal in-process MQTT broker for the e2e coverage run.
//
// The Playwright replay harness normally boots the app WITHOUT --mqtt, so the
// entire MQTT layer (MqttClient connect/CONNACK/recv-loop/flush, MqttIntegration
// state publishing, MqttHub, HA auto-discovery) never executes and stays
// uncovered. This broker gives the app's MqttClient a real MQTT 3.1.1 / 5.0 peer
// to connect to on loopback, so those paths run under the gcov-instrumented
// e2e build. It is a COVERAGE aid, not a functional assertion target — its only
// job is to accept the connection, honour SUBSCRIBE, and deliver PUBLISHes.
//
// Started as a Playwright `webServer` entry (Playwright waits for the TCP port);
// stops cleanly on SIGTERM so it doesn't linger between runs.

import { createServer } from 'node:net';
import Aedes from 'aedes';

const PORT = Number(process.env.AQUALINK_MQTT_PORT ?? 11883);
const HOST = '127.0.0.1';

const broker = new Aedes();
const server = createServer(broker.handle);

broker.on('client', (c) => console.log(`[mqtt-broker] connected: ${c?.id ?? '<anon>'}`));
broker.on('subscribe', (subs, c) =>
  console.log(`[mqtt-broker] ${c?.id ?? '<anon>'} subscribed: ${subs.map((s) => s.topic).join(', ')}`),
);
broker.on('publish', (packet, c) => {
  // Ignore the broker's own $SYS keep-alive traffic; log app publishes only.
  if (c && packet.topic && !packet.topic.startsWith('$SYS')) {
    console.log(`[mqtt-broker] publish ${packet.topic} (${packet.payload?.length ?? 0}b)`);
  }
});

server.listen(PORT, HOST, () => console.log(`[mqtt-broker] listening on mqtt://${HOST}:${PORT}`));

const shutdown = () => {
  server.close(() => broker.close(() => process.exit(0)));
  // Hard bound in case a socket lingers.
  setTimeout(() => process.exit(0), 3_000).unref();
};
process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);
