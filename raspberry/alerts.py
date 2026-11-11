"""
BIOSENSE - Alertas por Discord (se ejecuta en la Raspberry, sin pasar por ThingsBoard).

* Cada minuto (mediana del tramo) se clasifica cada variable en NORMAL / ADVERTENCIA / CRITICO.
* Se avisa solo cuando el estado CAMBIA y se mantiene ALERT_CONFIRM minutos seguidos (evita falsas alarmas).
* Si sigue en CRITICO se repite el aviso cada ALERT_REMIND_MIN minutos.
* Avisa cuando vuelve a NORMAL, cuando un sensor no entrega lectura valida y cuando deja de llegar data del TX.
"""
import json
import logging
import os
import queue
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

log = logging.getLogger("biosense.alerts")

NORMAL, WARN, CRIT, NODATA = "NORMAL", "ADVERTENCIA", "CRITICO", "SIN_DATO"

WEBHOOK_URL = os.getenv("DISCORD_WEBHOOK_URL", "").strip()
ALERT_CONFIRM = int(os.getenv("ALERT_CONFIRM", "2"))                 # minutos seguidos para confirmar un cambio
ALERT_REMIND_MIN = int(os.getenv("ALERT_REMIND_MIN", "30"))          # repetir si sigue en CRITICO
NO_DATA_ALERT_S = int(os.getenv("NO_DATA_ALERT_S", "300"))           # sin datos del TX
ALERT_TZ = ZoneInfo(os.getenv("ALERT_TZ", "America/Bogota"))         # hora que se muestra en Discord


# ----------------------------------------------------------------- umbrales
def _tds(v):
    return NORMAL if 0 <= v <= 200 else CRIT


def _sst(v):
    if v < 2500: return CRIT
    if v < 3000: return WARN
    if v <= 3500: return NORMAL
    if v <= 4000: return WARN
    return CRIT


def _od(v):
    if v < 1: return CRIT
    if v < 2: return WARN
    if v <= 3: return NORMAL
    if v <= 4: return WARN
    return CRIT


def _temp(v):
    if v < 14: return CRIT
    if v < 16: return WARN
    if v <= 22: return NORMAL
    if v <= 24: return WARN
    return CRIT


def _ph(v):
    if v < 6.8: return CRIT
    if v < 7.1: return WARN
    if v <= 8.0: return NORMAL
    if v <= 8.4: return WARN
    return CRIT


# clave -> (nombre, unidad, funcion, rango normal en texto)
VARIABLES = {
    "temp_c":  ("Temperatura",        "°C",   _temp, "16 a 22 °C"),
    "tds_ppm": ("TDS",                "ppm",  _tds,  "0 a 200 ppm"),
    "ph":      ("pH",                 "",     _ph,   "7.1 a 8.0"),
    "od_mgl":  ("Oxígeno disuelto",   "mg/L", _od,   "2 a 3 mg/L"),
    "sst_mgl": ("SST",                "mg/L", _sst,  "3000 a 3500 mg/L"),
}

COLOR = {NORMAL: 0x2ECC71, WARN: 0xF1C40F, CRIT: 0xE74C3C, NODATA: 0x95A5A6}
ICON = {NORMAL: "🟢", WARN: "🟡", CRIT: "🔴", NODATA: "⚪"}


def classify(key: str, value):
    if value is None:
        return NODATA
    return VARIABLES[key][2](value)


# ----------------------------------------------------------------- envio a Discord
class Discord:
    """Envia mensajes al webhook en un hilo aparte (no bloquea la adquisicion) y reintenta si no hay internet."""

    def __init__(self, url: str):
        self.url = url
        self.q = queue.Queue(maxsize=200)
        self.stop = False
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def send(self, title: str, text: str, level: str):
        embed = {
            "title": title,
            "description": text,
            "color": COLOR.get(level, COLOR[NODATA]),
            "timestamp": datetime.now(timezone.utc).isoformat(),
        }
        try:
            self.q.put_nowait({"embeds": [embed]})
        except queue.Full:
            try:
                self.q.get_nowait()          # descarta la mas vieja
                self.q.put_nowait({"embeds": [embed]})
            except (queue.Empty, queue.Full):
                pass

    def _post(self, payload: dict) -> float:
        """Devuelve 0 si se envio, o los segundos que hay que esperar para reintentar."""
        req = urllib.request.Request(
            self.url, data=json.dumps(payload).encode("utf-8"), method="POST",
            headers={"Content-Type": "application/json",
                     "User-Agent": "DiscordBot (biosense-edge, 1.0)"})
        try:
            with urllib.request.urlopen(req, timeout=10):
                return 0
        except urllib.error.HTTPError as e:
            if e.code == 429:                                   # limite de Discord
                try:
                    return float(json.loads(e.read().decode()).get("retry_after", 5)) + 0.5
                except Exception:
                    return 5
            if 400 <= e.code < 500:                             # URL mala o webhook borrado: no tiene sentido reintentar
                log.error("Discord rechazo el mensaje (HTTP %s). Revisa DISCORD_WEBHOOK_URL", e.code)
                return 0
            return 15                                           # error de Discord: reintentar
        except (urllib.error.URLError, OSError) as e:
            log.warning("Discord no disponible (%s); se reintentara", getattr(e, "reason", e))
            return 20                                           # sin internet

    def _run(self):
        while not self.stop:
            try:
                payload = self.q.get(timeout=1)
            except queue.Empty:
                continue
            while not self.stop:
                wait = self._post(payload)
                if wait == 0:
                    break
                time.sleep(wait)

    def close(self):
        self.stop = True


def make_notifier():
    if not WEBHOOK_URL:
        log.warning("DISCORD_WEBHOOK_URL vacio: alertas por Discord desactivadas")
        return None
    log.info("Alertas por Discord activas")
    return Discord(WEBHOOK_URL)


# ----------------------------------------------------------------- logica de alertas
class AlertEngine:
    def __init__(self, notifier, confirm=None, remind_s=None):
        self.n = notifier
        self.confirm = confirm if confirm is not None else ALERT_CONFIRM
        self.remind_s = remind_s if remind_s is not None else ALERT_REMIND_MIN * 60
        # se parte de NORMAL: si al arrancar ya esta mal, avisa tras confirmarse
        self.st = {k: {"level": NORMAL, "pending": None, "count": 0, "last_notice": 0.0} for k in VARIABLES}
        self.no_data = False

    @staticmethod
    def _hora(ts_epoch):
        return datetime.fromtimestamp(ts_epoch, ALERT_TZ).strftime("%d/%m %H:%M")

    def _notify(self, title, text, level):
        log.info("ALERTA: %s | %s", title, text.replace("\n", " "))
        if self.n:
            self.n.send(title, text, level)

    def evaluate(self, summary: dict, ts_epoch: float):
        """summary: medianas del tramo (puede faltar una variable si el sensor no dio lectura valida)."""
        for key, (name, unit, _fn, normal_txt) in VARIABLES.items():
            value = summary.get(key)
            lvl = classify(key, value)
            st = self.st[key]
            shown = f"{value:g} {unit}".strip() if value is not None else "sin lectura válida"

            if lvl == st["level"]:
                st["pending"], st["count"] = None, 0
                if lvl == CRIT and ts_epoch - st["last_notice"] >= self.remind_s:
                    st["last_notice"] = ts_epoch
                    self._notify(f"{ICON[lvl]} {name} sigue en CRÍTICO",
                                 f"Valor: **{shown}**\nRango normal: {normal_txt}\n{self._hora(ts_epoch)}", lvl)
                continue

            if st["pending"] == lvl:
                st["count"] += 1
            else:
                st["pending"], st["count"] = lvl, 1
            if st["count"] < self.confirm:
                continue

            before = st["level"]
            st["level"], st["pending"], st["count"], st["last_notice"] = lvl, None, 0, ts_epoch
            if lvl == NORMAL:
                self._notify(f"{ICON[NORMAL]} {name} volvió a NORMAL",
                             f"Valor: **{shown}**\nAntes: {before}\n{self._hora(ts_epoch)}", lvl)
            elif lvl == NODATA:
                self._notify(f"{ICON[NODATA]} {name}: sensor sin lectura válida",
                             f"Revisa el sensor o su cableado.\n{self._hora(ts_epoch)}", lvl)
            else:
                etiqueta = "CRÍTICO" if lvl == CRIT else "ADVERTENCIA"
                self._notify(f"{ICON[lvl]} {name} en {etiqueta}",
                             f"Valor: **{shown}**\nRango normal: {normal_txt}\n{self._hora(ts_epoch)}", lvl)

    def check_no_data(self, now_epoch: float, last_data_epoch: float):
        silent = now_epoch - last_data_epoch
        if silent > NO_DATA_ALERT_S and not self.no_data:
            self.no_data = True
            self._notify(f"{ICON[CRIT]} Sin datos del sensor",
                         f"No llegan lecturas hace {int(silent // 60)} min (TX apagado, sin LoRa, RX sin WiFi o sin broker).\n"
                         f"{self._hora(now_epoch)}", CRIT)
        elif silent <= NO_DATA_ALERT_S and self.no_data:
            self.no_data = False
            self._notify(f"{ICON[NORMAL]} Datos del sensor de nuevo",
                         f"Se reanudó la recepción.\n{self._hora(now_epoch)}", NORMAL)
