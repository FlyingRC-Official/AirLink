#!/usr/bin/env python3
"""AirLink C5 V1 USB factory-test runner."""
from __future__ import annotations
import argparse, csv, json, os, re, secrets, time
from datetime import datetime, timezone
from pathlib import Path
import serial

ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"

def command(port: serial.Serial, text: str, timeout: float = 15.0) -> dict:
    port.reset_input_buffer(); port.write((text + "\n").encode()); port.flush()
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        line=port.readline().decode(errors="replace").strip()
        if line.startswith("{"):
            return json.loads(line)
    raise TimeoutError(text)

def main() -> int:
    p=argparse.ArgumentParser();p.add_argument("--port",required=True);p.add_argument("--peer-port")
    p.add_argument("--serial",required=True);p.add_argument("--operator",required=True)
    p.add_argument("--uart-bytes",type=int,default=65536)
    p.add_argument("--output",type=Path,default=Path("factory-results"));p.add_argument("--skip-manual",action="store_true")
    a=p.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,23}", a.serial):
        raise SystemExit("Serial must be 1-24 ASCII letters, digits, dot, underscore, or hyphen")
    a.output.mkdir(mode=0o700,parents=True,exist_ok=True);a.output.chmod(0o700)
    private_path=a.output/f"{a.serial}.json";csv_path=a.output/"results.csv"
    if private_path.exists():raise SystemExit(f"Refusing to overwrite existing record: {private_path}")
    if csv_path.exists():
        with csv_path.open(newline="",encoding="utf-8") as f:
            if any(row.get("serial")==a.serial for row in csv.DictReader(f)):
                raise SystemExit(f"Duplicate serial in {csv_path}: {a.serial}")
    password="".join(secrets.choice(ALPHABET) for _ in range(16));results=[]
    with serial.Serial(a.port,115200,timeout=.5) as dev:
        time.sleep(1);info=command(dev,"factory info");results.append(info)
        results.append(command(dev,f"factory identity {a.serial} {password}"))
        results.append(command(dev,"factory nvs"));scan=command(dev,"factory wifi-scan",30);results.append(scan)
        results.append({"test":"wifi_bands","ok":scan.get("aps_2g",0)>0 and scan.get("aps_5g",0)>0,
                        "aps_2g":scan.get("aps_2g",0),"aps_5g":scan.get("aps_5g",0)})
        for baud in (57600,115200,230400,460800,921600):
            results.append(command(dev,f"factory uart-prbs {baud} {a.uart_bytes}",60))
        results.append(command(dev,"factory ble",10))
        if a.peer_port:
            with serial.Serial(a.peer_port,115200,timeout=.5) as peer:
                for bitrate in (125000,250000,500000,1000000):
                    local_rate=command(dev,f"factory can-bitrate {bitrate}")
                    peer_rate=command(peer,f"factory can-bitrate {bitrate}")
                    before=command(peer,"factory info")["can_rx"]
                    sent=command(dev,"factory can-send 123 5a");time.sleep(.5)
                    after=command(peer,"factory info")["can_rx"]
                    results.append({"test":"can_pair","bitrate":bitrate,
                                    "ok":local_rate.get("ok") and peer_rate.get("ok") and sent.get("ok") and after>before,
                                    "before":before,"after":after})
        else:
            results.append({"test":"can_pair","ok":False,"error":"peer_port_required_for_PASS"})
        if not a.skip_manual:
            input("Hold BOOT, then press Enter (keep holding). ")
            results.append(command(dev,"factory boot"))
            results[-1]["ok"]=bool(results[-1].get("pressed"))
            input("Release BOOT, then press Enter. ")
            results.append(command(dev,"factory led blue"));results.append(command(dev,"factory act"))
            if input("Are PWR, ACT pulse, and RGB indicators physically correct? [y/N] ").lower()!="y":
                results.append({"test":"indicator_visual","ok":False})
            for color in ("red","green","blue","white"):
                results.append(command(dev,f"factory led {color}"))
                if input(f"Is RGB LED {color}? [y/N] ").lower()!="y":results.append({"test":f"led_visual_{color}","ok":False})
        else:
            results.append({"test":"manual_checks","ok":False,"error":"manual_checks_skipped"})
    passed=(all(r.get("ok",False) for r in results) and info.get("chip") and info.get("usb") and
            info.get("flash_bytes")==8388608 and info.get("psram_bytes")==8388608)
    record={"serial":a.serial,"operator":a.operator,"timestamp":datetime.now(timezone.utc).isoformat(),"port":a.port,
            "initial_password":password,"passed":bool(passed),"results":results}
    fd=os.open(private_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,"w",encoding="utf-8") as f:
        json.dump(record,f,indent=2)
        f.write("\n")
    new=not csv_path.exists()
    with csv_path.open("a",newline="",encoding="utf-8") as f:
        w=csv.DictWriter(f,fieldnames=["serial","timestamp","operator","passed","hardware_id"])
        if new:w.writeheader()
        w.writerow({"serial":a.serial,"timestamp":record["timestamp"],"operator":a.operator,"passed":passed,"hardware_id":"airlink-c5-mesh-v1"})
    print(json.dumps({"serial":a.serial,"passed":passed,"json":str(private_path)}));return 0 if passed else 1
if __name__=="__main__":raise SystemExit(main())
