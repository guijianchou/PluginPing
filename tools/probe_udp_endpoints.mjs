import crypto from 'node:crypto';
import dgram from 'node:dgram';

function probe(kind, host, port, timeoutMs = 5000) {
  return new Promise((resolve) => {
    const socket = dgram.createSocket('udp4');
    let finished = false;
    const transaction = crypto.randomBytes(12);
    let payload;
    if (kind === 'echo') {
      payload = Buffer.concat([Buffer.from('PPMECHO1'), crypto.randomBytes(24)]);
    } else {
      payload = Buffer.alloc(20);
      payload.writeUInt16BE(0x0001, 0);
      payload.writeUInt32BE(0x2112a442, 4);
      transaction.copy(payload, 8);
    }

    const started = Date.now();
    const finish = (result) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      socket.close();
      resolve({ kind, host, port, ...result });
    };
    const timer = setTimeout(() => finish({ ok: false, error: 'timeout' }), timeoutMs);

    socket.on('error', (error) => finish({ ok: false, error: error.message }));
    socket.on('message', (message) => {
      const ok = kind === 'echo'
        ? message.equals(payload)
        : message.length >= 20 &&
          message.readUInt16BE(0) === 0x0101 &&
          message.readUInt32BE(4) === 0x2112a442 &&
          message.subarray(8, 20).equals(transaction);
      finish({ ok, rttMs: Date.now() - started, bytes: message.length });
    });
    socket.send(payload, port, host, (error) => {
      if (error) finish({ ok: false, error: error.message });
    });
  });
}

const results = await Promise.all([
  probe('echo', 'sg.falsemeet.site', 3478),
  probe('stun', 'stun.cloudflare.com', 3478),
]);

console.log(JSON.stringify(results, null, 2));
process.exitCode = results.every((result) => result.ok) ? 0 : 1;
