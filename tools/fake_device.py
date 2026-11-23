"""
Eszköz nélküli teszt: úgy viselkedik, mint az ESP32 HTTP módban.
Véletlenszerű, de valószerű méréseket küld az InfluxDB-be ugyanabban
a line protocol formátumban, mint a firmware.

Használat (csak Python 3 kell, külső csomag nem):
    python tools/fake_device.py --token IDE_A_TOKEN
    python tools/fake_device.py --token ... --interval 5 --url http://localhost:8086
"""
import argparse
import math
import random
import time
import urllib.error
import urllib.request

p = argparse.ArgumentParser()
p.add_argument("--url", default="http://localhost:8086")
p.add_argument("--org", default="iothw")
p.add_argument("--bucket", default="airquality")
p.add_argument("--token", required=True)
p.add_argument("--device", default="fake-esp32")
p.add_argument("--interval", type=float, default=10, help="küldési periódus (s)")
args = p.parse_args()

write_url = f"{args.url}/api/v2/write?org={args.org}&bucket={args.bucket}&precision=ns"
print(f"Küldés ide: {write_url}  (Ctrl+C a leállításhoz)")

t0 = time.time()
while True:
    phase = (time.time() - t0) / 600  # lassú "napi" ingadozás szimulálása
    temp = 21 + 2 * math.sin(phase) + random.uniform(-0.2, 0.2)
    rh = 50 + 15 * math.sin(phase + 1) + random.uniform(-1, 1)
    pres = 1013 + random.uniform(-0.5, 0.5)
    pm25 = max(0.0, 12 + 8 * math.sin(phase * 2) + random.uniform(-2, 2))
    pm1, pm10 = pm25 * 0.7, pm25 * 1.4

    line = (
        f"air_quality,device={args.device} "
        f"temperature={temp:.2f},humidity={rh:.2f},pressure={pres:.2f},"
        f"pm1_0={pm1:.1f},pm2_5={pm25:.1f},pm2_5_corr={pm25:.1f},"
        f"pm10={pm10:.1f},pm10_corr={pm10:.1f},samples=10i,rssi=-60i "
        f"{time.time_ns()}"
    )
    req = urllib.request.Request(
        write_url,
        data=line.encode(),
        method="POST",
        headers={"Authorization": f"Token {args.token}",
                 "Content-Type": "text/plain; charset=utf-8"},
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            print(f"[{r.status}] T={temp:.1f}C RH={rh:.0f}% PM2.5={pm25:.1f}")
    except urllib.error.HTTPError as e:
        print(f"[HIBA {e.code}] {e.read().decode(errors='replace')}")
    except urllib.error.URLError as e:
        print(f"[HIBA] Nem érhető el az InfluxDB: {e.reason}")
    time.sleep(args.interval)
