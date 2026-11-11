#!/usr/bin/env python3
"""
BIOSENSE - Nodo borde (Raspberry Pi)

Entrada (INPUT_MODE):
  mqtt   (por defecto) : la Heltec RX publica por WiFi en el broker Mosquitto de esta Raspberry
  serial               : la Heltec RX conectada por USB a la Raspberry

Flujo:  entrada -> validar/limpiar -> CSV local (cada lectura)
                -> mediana por ventana -> ThingsBoard Cloud (MQTT)

Payload MQTT (topico biosense/rx/data):  SEQ,FECHA_HORA,TEMP,TDS,PH,OD,SST,RSSI,SNR
  (tambien acepta el formato anterior de 7 numeros: TEMP,TDS,PH,OD,SST,RSSI,SNR)
Linea serial:                            DATA,<lo mismo>

Persistencia:
  * Cada lectura se ubica por la HORA QUE PUSO EL TX (RTC), no por la hora de llegada.
    Asi, las lecturas atrasadas que el TX reenvia desde su SD caen en su minuto correcto.
  * Lecturas repetidas (mismo instante) se descartan.
  * Si no hay internet, los minutos pendientes se vuelven a subir solos al volver la conexion.
"""
import csv
import json
import logging
import os
import queue
import signal
import statistics
import time
from collections import OrderedDict
from datetime import datetime, timezone
from zoneinfo import ZoneInfo
from pathlib import Path

import paho.mqtt.client as mqtt

import alerts

# ----------------------------------------------------------------- configuracion
INPUT_MODE = os.getenv("INPUT_MODE", "mqtt").lower()          # mqtt | serial
DATA_DIR = Path(os.getenv("BIOSENSE_DATA_DIR", str(Path.home() / "biosense_data")))

# Broker local (Mosquitto en esta Raspberry)
LOCAL_HOST = os.getenv("LOCAL_MQTT_HOST", "localhost")
LOCAL_PORT = int(os.getenv("LOCAL_MQTT_PORT", "1883"))
LOCAL_USER = os.getenv("LOCAL_MQTT_USER", "biosense")
LOCAL_PASS = os.getenv("LOCAL_MQTT_PASS", "")
LOCAL_TOPIC = os.getenv("LOCAL_MQTT_TOPIC", "biosense/rx/data")

# Modo serial (opcional)
SERIAL_PORT = os.getenv("BIOSENSE_PORT", "/dev/ttyUSB0")
BAUD = 115200

# ThingsBoard Cloud
TB_HOST = os.getenv("TB_HOST", "thingsboard.cloud")   # EU: eu.thingsboard.cloud
TB_PORT = int(os.getenv("TB_PORT", "8883"))           # 8883 = TLS; si no conecta prueba 1883
TB_TOKEN = os.getenv("TB_TOKEN", "")                  # Access token del dispositivo
TB_TOPIC = "v1/devices/me/telemetry"

CLOUD_INTERVAL_S = int(os.getenv("CLOUD_INTERVAL_S", "60"))   # cada cuanto se sube un dato
NO_DATA_WARN_S = int(os.getenv("NO_DATA_WARN_S", "60"))       # aviso si no llega nada
SEND_LINK_QUALITY = False   # True para subir tambien RSSI/SNR a la nube

# Hora del RTC del TX: zona horaria en la que esta ajustado el DS3231 (Colombia no tiene horario de verano)
TX_TZ = ZoneInfo(os.getenv("TX_TZ", "America/Bogota"))
LATE_GRACE_S = int(os.getenv("LATE_GRACE_S", "5"))            # espera tras cerrar cada minuto antes de subirlo
FLUSH_EVERY_S = int(os.getenv("FLUSH_EVERY_S", "5"))          # cada cuanto se revisan minutos pendientes
MAX_PUBLISH_PER_FLUSH = 20                                    # tope de minutos subidos por revision
ALERT_MAX_AGE_S = 300     # no se alerta por tramos mas viejos que esto (datos recuperados tras una caida)
KEEP_BUCKETS_S = 3 * 24 * 3600                                # cuanto se recuerda cada minuto en memoria

FIELDS = ["temp_c", "tds_ppm", "ph", "od_mgl", "sst_mgl"]
LINK_FIELDS = ["rssi_dbm", "snr_db"]

# Rangos fisicamente razonables. Fuera de rango => se marca y no se sube a la nube.
LIMITS = {
    "temp_c":  (-10.0, 80.0),
    "tds_ppm": (0.0, 5000.0),
    "ph":      (0.0, 14.0),
    "od_mgl":  (0.0, 25.0),
    "sst_mgl": (0.0, 5000.0),
}
TEMP_ERROR_VALUE = -127.0   # lo que envia el TX si el DS18B20 falla
# El TX envia pH = -1 si el modulo esta apagado: queda fuera de rango y se marca invalido.

log = logging.getLogger("biosense")


# ----------------------------------------------------------------- logica pura
def parse_tx_time(text: str):
    """'2026-10-27 21:58:00' (hora del RTC del TX) -> datetime UTC, o None si no es creible."""
    try:
        dt = datetime.strptime(text.strip(), "%Y-%m-%d %H:%M:%S").replace(tzinfo=TX_TZ)
    except ValueError:
        return None                       # p.ej. 'RTC_ERROR'
    dt = dt.astimezone(timezone.utc)
    now = datetime.now(timezone.utc)
    if dt.year < 2025 or (dt - now).total_seconds() > 24 * 3600:
        return None                       # RTC sin ajustar o con hora absurda
    return dt


def parse_fields(text: str):
    """'seq,fecha,t,tds,ph,od,sst,rssi,snr' (o 7 numeros) -> dict, o None si no es valida."""
    parts = text.strip().split(",")
    seq, tx_ts = None, None
    if len(parts) == 9:
        try:
            seq = int(parts[0])
        except ValueError:
            return None
        tx_ts = parse_tx_time(parts[1])
        parts = parts[2:]
    if len(parts) != 7:
        return None
    try:
        vals = [float(p) for p in parts]
    except ValueError:
        return None
    out = dict(zip(FIELDS + LINK_FIELDS, vals))
    out["seq"], out["tx_ts"] = seq, tx_ts
    return out


def parse_line(line: str):
    """Linea serial con prefijo 'DATA,' (ignora cualquier otra cosa)."""
    idx = line.find("DATA,")
    if idx < 0:
        return None
    return parse_fields(line[idx + 5:])


def validate(sample: dict):
    """Devuelve (valores_limpios, lista_de_campos_invalidos). Invalido => None."""
    clean, bad = {}, []
    for f in FIELDS:
        v = sample[f]
        lo, hi = LIMITS[f]
        if (f == "temp_c" and v == TEMP_ERROR_VALUE) or not (lo <= v <= hi):
            clean[f] = None
            bad.append(f)
        else:
            clean[f] = v
    return clean, bad


class Window:
    """Acumula lecturas limpias y entrega la mediana de cada variable."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.data = {f: [] for f in FIELDS + LINK_FIELDS}
        self.count = 0

    def add(self, clean: dict, sample: dict):
        self.count += 1
        for f in FIELDS:
            if clean[f] is not None:
                self.data[f].append(clean[f])
        for f in LINK_FIELDS:
            self.data[f].append(sample[f])

    def summary(self):
        """None si no hubo ninguna lectura valida en la ventana."""
        out = {f: round(statistics.median(v), 2) for f, v in self.data.items() if v}
        if not any(f in out for f in FIELDS):
            return None
        out["muestras"] = self.count
        return out


class Buckets:
    """Una Window por cada tramo de CLOUD_INTERVAL_S segundos, ubicada por la hora de la lectura."""

    def __init__(self, width: int):
        self.width = width
        self.win = {}
        self.dirty = set()

    def add(self, ts_epoch: float, clean: dict, sample: dict):
        start = int(ts_epoch // self.width * self.width)
        self.win.setdefault(start, Window()).add(clean, sample)
        self.dirty.add(start)

    def ready(self, now_epoch: float, grace: int):
        """Tramos con datos nuevos y ya cerrados (incluye los atrasados)."""
        return sorted(st for st in self.dirty if st + self.width + grace <= now_epoch)

    def summary(self, start: int):
        return self.win[start].summary()

    def mark_clean(self, start: int):
        self.dirty.discard(start)

    def prune(self, now_epoch: float, keep_s: int):
        for st in [st for st in self.win if st + keep_s < now_epoch and st not in self.dirty]:
            del self.win[st]


class CsvLogger:
    HEADER = ["timestamp_utc"] + FIELDS + LINK_FIELDS + ["campos_invalidos", "seq", "recibido_utc"]

    def __init__(self, folder: Path):
        self.folder = folder
        self.folder.mkdir(parents=True, exist_ok=True)

    def write(self, ts: datetime, sample: dict, bad: list, received: datetime):
        path = self.folder / f"datos_{ts.strftime('%Y-%m-%d')}.csv"
        is_new = not path.exists() or path.stat().st_size == 0
        with open(path, "a", newline="") as fh:
            w = csv.writer(fh)
            if is_new:
                w.writerow(self.HEADER)
            w.writerow([ts.isoformat(timespec="seconds")]
                       + [sample[f] for f in FIELDS + LINK_FIELDS]
                       + [";".join(bad), sample.get("seq", ""), received.isoformat(timespec="seconds")])
            fh.flush()
            os.fsync(fh.fileno())


# ----------------------------------------------------------------- MQTT (utilidades)
def new_client():
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)   # paho-mqtt 2.x
    except AttributeError:
        return mqtt.Client()                                   # paho-mqtt 1.x


# ----------------------------------------------------------------- fuentes de datos
class MqttSource:
    """Se suscribe al broker local; deja las muestras en una cola."""

    def __init__(self):
        self.q = queue.Queue(maxsize=1000)
        self.client = new_client()
        if LOCAL_USER:
            self.client.username_pw_set(LOCAL_USER, LOCAL_PASS)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)
        self.client.connect_async(LOCAL_HOST, LOCAL_PORT, keepalive=60)
        self.client.loop_start()

    def _on_connect(self, c, userdata, flags, rc, *args):
        # rc puede ser int (paho 1.x) o ReasonCode (2.x)
        failed = getattr(rc, "is_failure", None)
        failed = failed if isinstance(failed, bool) else (rc != 0)
        if failed:
            log.error("Broker local rechazo la conexion (rc=%s): revisa usuario/clave", rc)
            return
        log.info("Broker local conectado (%s:%s), suscrito a %s", LOCAL_HOST, LOCAL_PORT, LOCAL_TOPIC)
        c.subscribe(LOCAL_TOPIC, qos=1)   # se re-suscribe tras cada reconexion

    def _on_disconnect(self, c, userdata, *args):
        log.warning("Broker local desconectado; se reintentara solo")

    def _on_message(self, c, userdata, msg):
        text = msg.payload.decode("utf-8", errors="ignore")
        sample = parse_fields(text)
        if sample is None:
            log.warning("Payload MQTT con formato inesperado: %r", text[:80])
            return
        try:
            self.q.put_nowait(sample)
        except queue.Full:
            log.warning("Cola llena, se descarta una lectura")

    def get(self, timeout):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        self.client.loop_stop()
        self.client.disconnect()


class SerialSource:
    """Lee lineas 'DATA,...' del puerto serie (Heltec por USB)."""

    def __init__(self):
        import serial   # solo hace falta en este modo
        self.serial = serial
        self.ser = None

    def _open(self):
        s = self.serial.Serial()
        s.port, s.baudrate, s.timeout = SERIAL_PORT, BAUD, 1
        s.dtr = False   # evita reiniciar el ESP32 al abrir el puerto
        s.rts = False
        s.open()
        return s

    def get(self, timeout):
        if self.ser is None:
            try:
                self.ser = self._open()
                log.info("Puerto serie abierto")
            except (self.serial.SerialException, OSError) as e:
                log.error("No se puede abrir %s (%s). Reintento en 5 s", SERIAL_PORT, e)
                time.sleep(5)
                return None
        try:
            raw = self.ser.readline()
        except (self.serial.SerialException, OSError) as e:
            log.error("Puerto serie perdido (%s)", e)
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
            return None
        return parse_line(raw.decode("utf-8", errors="ignore")) if raw else None

    def close(self):
        if self.ser:
            self.ser.close()


# ----------------------------------------------------------------- nube (ThingsBoard)
def make_cloud():
    if not TB_TOKEN:
        log.warning("TB_TOKEN vacio: modo SOLO LOCAL (no se envia a la nube)")
        return None
    client = new_client()
    client.username_pw_set(TB_TOKEN)
    if TB_PORT == 8883:
        client.tls_set()   # usa los certificados del sistema

    def on_connect(c, userdata, flags, rc, *args):
        log.info("ThingsBoard conectado a %s:%s (rc=%s)", TB_HOST, TB_PORT, rc)

    def on_disconnect(c, userdata, *args):
        log.warning("ThingsBoard desconectado; se reintentara solo")

    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.reconnect_delay_set(min_delay=2, max_delay=60)
    client.connect_async(TB_HOST, TB_PORT, keepalive=60)
    client.loop_start()
    return client


def publish_cloud(client, summary: dict, ts_epoch: float) -> bool:
    """True si el dato quedo entregado a la libreria MQTT (o no hay nube configurada).
    False si no hay conexion: el tramo queda pendiente y se reintenta."""
    if client is None:
        return True
    if not client.is_connected():
        return False
    values = {f: summary[f] for f in FIELDS if f in summary}
    if SEND_LINK_QUALITY:
        values.update({f: summary[f] for f in LINK_FIELDS if f in summary})
    values["muestras"] = summary["muestras"]
    payload = {"ts": int(ts_epoch * 1000), "values": values}
    info = client.publish(TB_TOPIC, json.dumps(payload), qos=1)
    log.info("Nube <- %s @ %s (rc=%s)", values,
             datetime.fromtimestamp(ts_epoch, timezone.utc).strftime("%H:%M:%S"), info.rc)
    return info.rc == mqtt.MQTT_ERR_SUCCESS


def flush_cloud(cloud, buckets: Buckets, now_epoch: float):
    sent = 0
    for start in buckets.ready(now_epoch, LATE_GRACE_S):
        summary = buckets.summary(start)
        if summary is not None:
            if not publish_cloud(cloud, summary, start + buckets.width):
                log.warning("Sin conexion con la nube: %d tramo(s) pendiente(s), se reintentara", len(buckets.dirty))
                return
            sent += 1
        buckets.mark_clean(start)
        if sent >= MAX_PUBLISH_PER_FLUSH:
            return
    buckets.prune(now_epoch, KEEP_BUCKETS_S)


def run_alerts(engine, buckets: Buckets, evaluated: set, now_epoch: float):
    """Evalua umbrales de cada tramo (minuto) ya cerrado, una sola vez por tramo."""
    for start in sorted(buckets.win):
        if start in evaluated or start + buckets.width + LATE_GRACE_S > now_epoch:
            continue
        evaluated.add(start)
        end = start + buckets.width
        if now_epoch - end > ALERT_MAX_AGE_S:
            continue                          # dato atrasado (recuperado): se guarda pero no genera alerta
        engine.evaluate(buckets.summary(start) or {}, end)
    for st in [x for x in evaluated if x + KEEP_BUCKETS_S < now_epoch]:
        evaluated.discard(st)


# ----------------------------------------------------------------- principal
def main():
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    csv_log = CsvLogger(DATA_DIR)
    cloud = make_cloud()
    source = SerialSource() if INPUT_MODE == "serial" else MqttSource()
    buckets = Buckets(CLOUD_INTERVAL_S)
    notifier = alerts.make_notifier()
    engine = alerts.AlertEngine(notifier)
    evaluated = set()
    last_data_epoch = time.time()
    engine._notify("🟢 Monitor BIOSENSE iniciado", "La Raspberry está recibiendo y vigilando los datos.", alerts.NORMAL)
    seen = OrderedDict()          # instantes ya procesados (para descartar repetidas)
    next_flush = time.monotonic() + FLUSH_EVERY_S
    last_data = time.monotonic()
    warned = False
    log.info("Entrada: %s | CSV en %s | nube: tramos de %ss | zona del RTC del TX: %s",
             INPUT_MODE, DATA_DIR, CLOUD_INTERVAL_S, TX_TZ.key)

    while running:
        sample = source.get(timeout=1.0)
        now_m = time.monotonic()

        if sample is not None:
            last_data, warned = now_m, False
            last_data_epoch = time.time()
            received = datetime.now(timezone.utc)          # hora de la Raspberry
            tx_ts = sample.get("tx_ts")
            ts = tx_ts or received                          # sin hora del TX se usa la de llegada
            key = int(ts.timestamp()) if tx_ts else None

            if key is not None and key in seen:
                log.info("Lectura repetida (SEQ %s), se descarta", sample.get("seq"))
            else:
                if key is not None:
                    seen[key] = True
                    while len(seen) > 20000:
                        seen.popitem(last=False)
                clean, bad = validate(sample)
                csv_log.write(ts, sample, bad, received)
                buckets.add(ts.timestamp(), clean, sample)
                atras = (received - ts).total_seconds()
                log.info("Lectura SEQ %s %s%s | invalidos: %s | RSSI %s dBm",
                         sample.get("seq"), {f: sample[f] for f in FIELDS},
                         f" | ATRASADA {int(atras)} s" if atras > 30 else "",
                         bad or "ninguno", sample["rssi_dbm"])
        elif now_m - last_data > NO_DATA_WARN_S and not warned:
            log.warning("Sin lecturas hace mas de %ss (TX apagado, RX sin WiFi o sin broker)", NO_DATA_WARN_S)
            warned = True

        if now_m >= next_flush:
            now_e = time.time()
            run_alerts(engine, buckets, evaluated, now_e)
            engine.check_no_data(now_e, last_data_epoch)
            flush_cloud(cloud, buckets, now_e)
            next_flush = now_m + FLUSH_EVERY_S

    source.close()
    if notifier:
        notifier.close()
    if cloud:
        cloud.loop_stop()
        cloud.disconnect()
    log.info("Detenido")


if __name__ == "__main__":
    main()
