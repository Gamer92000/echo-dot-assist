// Zigbee2MQTT's library as the coordinator's host, for tests/fake_zigbee_z2m.py: node z2m.mjs <adapter> <tcp port>
// <folder> <seconds>.  Starts the controller (adapter "deconz": zigbeed's coordinator through the test's forwarder;
// "zoh": zigbee-herdsman's own stack on a simulated RCP), opens joining, prints one JSON line per event, stops after
// that many seconds (joining opened again halfway).
import { Controller } from "zigbee-herdsman";
import { setLogger } from "zigbee-herdsman/dist/utils/logger.js";
import fs from "node:fs";

const [adapter, port, dir, secs] = process.argv.slice(2);
const out = (o) => console.log(JSON.stringify(o));
const log = (lvl) => (msg, ns) => { if (lvl !== "debug") console.error(`[${lvl}] ${ns}: ${typeof msg === "function" ? msg() : msg}`); };
setLogger({ debug: log("debug"), info: log("info"), warning: log("warning"), error: log("error") });
fs.mkdirSync(dir, { recursive: true });
const c = new Controller({
  network: { panID: 0x1a62, extendedPanID: [0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd, 0xdd], channelList: [21],
             networkKey: [1, 3, 5, 7, 9, 11, 13, 15, 0, 2, 4, 6, 8, 10, 12, 13], networkKeyDistribute: false },
  transport: { path: `tcp://127.0.0.1:${port}`, adapter },
  databasePath: `${dir}/database.db`, databaseBackupPath: `${dir}/database.db.backup`, backupPath: `${dir}/coordinator_backup.json`,
  adapter: { disableLED: false }, acceptJoiningDeviceHandler: async () => true,
});
c.on("deviceJoined", (d) => out({ event: "joined", ieee: d.device.ieeeAddr }));
c.on("deviceInterview", (d) => out({ event: "interview", status: d.status, ieee: d.device.ieeeAddr, model: d.device.modelID, manufacturer: d.device.manufacturerName, type: d.device.type }));
c.on("deviceAnnounce", (d) => out({ event: "announce", ieee: d.device.ieeeAddr }));
try {
  const r = await c.start();
  out({ event: "started", result: r, coordinator: await c.getCoordinatorVersion(), network: await c.getNetworkParameters() });
  // joining opened, and again halfway: a router that joined meanwhile only opens on a permit it hears (Zigbee's
  // Mgmt_Permit_Joining_req broadcast), as a person presses "Permit join" again for a device that joins through it
  await c.permitJoin(Number(secs));
  await new Promise((res) => setTimeout(res, Number(secs) * 500));
  await c.permitJoin(Number(secs));
  await new Promise((res) => setTimeout(res, Number(secs) * 500));
  for (const d of c.getDevicesIterator((x) => x.type !== "Coordinator")) {
    try {
      const ep = d.getEndpoint(1);
      const r = await ep.read("genBasic", ["manufacturerName", "modelId"]);
      out({ event: "read", ieee: d.ieeeAddr, ...r });
    } catch (e) { out({ event: "read_failed", ieee: d.ieeeAddr, error: String(e) }); }
  }
} catch (e) {
  out({ event: "failed", error: String(e.stack || e) });
}
await c.stop().catch(() => {});
process.exit(0);
