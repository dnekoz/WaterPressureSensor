"""Collect temporary Wi-Fi diagnostics without changing device settings.

Example (password is prompted without echo):
    python scripts/capture_wifi_diagnostics.py --duration 600

For unattended collection, set a password environment variable and pass its
name with --password-env. Passwords and session cookies are never written.
Only /api/login is posted; all other requests are GETs.
"""

import argparse
from collections import Counter
from datetime import datetime, timezone
import getpass
import http.client
import http.cookiejar
import json
import os
from pathlib import Path
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
STATUS_INTERVAL = 5.0
DIAGNOSTIC_INTERVAL = 15.0
LOGIN_INTERVAL = 3.0
MAX_RESPONSE = 1024 * 1024


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def write_json_line(stream, value):
    stream.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")
    stream.flush()


def field(record, *names):
    for name in names:
        if name in record:
            return record[name]
    return None


def numeric(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


class Evidence:
    """Summarize observations, retaining numeric codes without guessing causes."""

    def __init__(self):
        self.events = Counter()
        self.sdk_statuses = Counter()
        self.reasons = Counter()
        self.pending_associations = {}
        self.association_outcomes = []
        self.retry_results = Counter()
        self.arduino_statuses = Counter()
        self.dhcp_states = Counter()
        self.modes = Counter()
        self.last_event_ms = {}
        self.dropped_events = {}
        self.got_ip_events = 0
        self.invalid_lines = 0
        self.unique_lines = 0
        self.last_status = None
        self.last_success_utc = None
        self.boot_changes = 0
        self.wifi_disconnected_polls = 0
        self.versions = Counter()
        self.outages = []
        self.open_outage = None

    def observe_status(self, status, captured_utc):
        previous = self.last_status
        uptime = status.get("uptime_s")
        if previous and numeric(uptime) and numeric(previous.get("uptime_s")):
            if uptime < previous["uptime_s"]:
                self.boot_changes += 1
        self.last_status = status
        self.last_success_utc = captured_utc
        wifi = status.get("wifi")
        if isinstance(wifi, dict) and wifi.get("connected") is False:
            self.wifi_disconnected_polls += 1
        if status.get("version"):
            self.versions[str(status["version"])] += 1
        if self.open_outage is not None:
            self.open_outage["http_returned_utc"] = captured_utc
            self.open_outage["returned_uptime_s"] = uptime
            self.open_outage = None

    def observe_failure(self, captured_utc, error):
        if self.open_outage is None:
            self.open_outage = {
                "first_failed_poll_utc": captured_utc,
                "last_success_utc": self.last_success_utc,
                "last_success_uptime_s": (self.last_status or {}).get("uptime_s"),
                "first_error": error,
            }
            self.outages.append(self.open_outage)
        self.open_outage["last_failed_poll_utc"] = captured_utc

    def observe_event(self, record):
        event = str(field(record, "event", "type", "kind") or "unknown")
        self.events[event] += 1
        sdk = field(record, "sdk_status", "sdk", "station_status")
        if numeric(sdk):
            self.sdk_statuses[str(sdk)] += 1
        for counter, key in ((self.arduino_statuses, "status"), (self.dhcp_states, "dhcp"), (self.modes, "mode")):
            value = record.get(key)
            if numeric(value):
                counter[str(value)] += 1
        reason = field(record, "reason", "disconnect_reason")
        if numeric(reason):
            self.reasons[str(reason)] += 1
        boot = str(field(record, "boot_id", "boot", "bootId") or "unknown")
        stamp = field(record, "t_ms", "ms", "uptime_ms", "millis")
        if numeric(stamp):
            self.last_event_ms[boot] = field(record, "sample_ms") or stamp
        dropped = record.get("dropped")
        if numeric(dropped):
            self.dropped_events[boot] = max(self.dropped_events.get(boot, 0), dropped)
        normalized = event.lower().replace("-", "_")
        if normalized == "retry" and isinstance(record.get("result"), bool):
            self.retry_results[str(record["result"]).lower()] += 1
        if normalized in ("associated", "sta_connected", "station_connected", "wifi_connected", "connected"):
            self.finish_association(boot, stamp, "another_connected_event")
            self.pending_associations[boot] = {"boot": boot, "connected_ms": stamp, "event_channel": record.get("event_channel")}
        elif normalized in ("got_ip", "sta_got_ip", "station_got_ip", "wifi_got_ip"):
            self.got_ip_events += 1
            self.finish_association(boot, stamp, "got_ip")
        elif normalized in ("disconnected", "sta_disconnected", "station_disconnected", "wifi_disconnected"):
            self.finish_association(boot, stamp, "disconnected_before_got_ip", reason)

    def finish_association(self, boot, stamp, outcome, reason=None):
        association = self.pending_associations.pop(boot, None)
        if association is None:
            return
        association.update({"outcome": outcome, "ended_ms": stamp})
        if numeric(stamp) and numeric(association["connected_ms"]):
            association["observed_elapsed_ms"] = (stamp - association["connected_ms"]) % (2 ** 32)
        if reason is not None:
            association["reason"] = reason
        self.association_outcomes.append(association)

    def summary(self):
        return {
            "unique_diagnostic_lines": self.unique_lines,
            "invalid_diagnostic_lines": self.invalid_lines,
            "event_counts": dict(self.events),
            "raw_sdk_status_counts": dict(self.sdk_statuses),
            "numeric_disconnect_reason_counts": dict(self.reasons),
            "arduino_status_counts": dict(self.arduino_statuses),
            "dhcp_state_counts": dict(self.dhcp_states),
            "wifi_mode_counts": dict(self.modes),
            "retry_return_value_counts": dict(self.retry_results),
            "reported_dropped_events_by_boot": self.dropped_events,
            "got_ip_events": self.got_ip_events,
            "association_outcomes": self.association_outcomes,
            "associations_without_later_got_ip_or_disconnect_in_captured_log": self.pending_associations,
            "last_observed_event_ms_by_boot": self.last_event_ms,
            "observed_uptime_decreases": self.boot_changes,
            "status_polls_reporting_wifi_disconnected": self.wifi_disconnected_polls,
            "firmware_version_counts": dict(self.versions),
            "http_outages": self.outages,
            "last_uptime_s": (self.last_status or {}).get("uptime_s"),
            "interpretation": (
                "HTTP failure alone does not establish Wi-Fi disconnection. "
                "Association without a later GOT_IP is evidence only within the captured log; "
                "rotation, missing events, and an unfinished connection remain possible. "
                "Numeric SDK statuses and disconnect reasons are retained without inferring a root cause."
            ),
        }


class Device:
    def __init__(self, base_url, password, timeout):
        self.base_url = base_url.rstrip("/")
        self.password = password
        self.timeout = timeout
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()),
        )
        self.last_login = float("-inf")

    def request(self, path, data=None):
        headers = {"Accept": "application/json, application/x-ndjson, text/plain"}
        if data is not None:
            headers.update({"Content-Type": "application/json", "X-WPS-Request": "1"})
        request = urllib.request.Request(self.base_url + path, data=data, headers=headers)
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                payload = response.read(MAX_RESPONSE + 1)
                if len(payload) > MAX_RESPONSE:
                    return None, None, "response_too_large"
                return response.status, payload, None
        except urllib.error.HTTPError as exc:
            # Do not log bodies/headers: they can contain session information.
            exc.close()
            return exc.code, None, "http_" + str(exc.code)
        except (urllib.error.URLError, TimeoutError, socket.timeout, OSError, http.client.HTTPException):
            return None, None, "network_or_timeout"

    def diagnostics(self):
        code, payload, error = self.request("/api/diagnostics/wifi")
        if code != 401:
            return code, payload, error
        now = time.monotonic()
        if now - self.last_login < LOGIN_INTERVAL:
            return code, None, "login_throttled"
        self.last_login = now
        body = json.dumps({"password": self.password}).encode("utf-8")
        login_code, _, login_error = self.request("/api/login", body)
        if login_code != 200:
            return login_code, None, "login_failed_" + (login_error or str(login_code))
        return self.request("/api/diagnostics/wifi")


def capture(args, password):
    started_utc = utc_now()
    folder_name = datetime.now(timezone.utc).strftime("wifi-diagnostics-%Y%m%dT%H%M%S.%fZ")
    output = (ROOT / ".pio" / "hardware" / folder_name).resolve()
    output.mkdir(parents=True, exist_ok=False)
    device = Device(args.url, password, args.timeout)
    evidence = Evidence()
    seen = set()
    started = time.monotonic()
    next_status = started
    next_diagnostic = started
    deadline = started + args.duration if args.duration else float("inf")
    last_console_state = None
    last_diagnostic_error = None
    status_polls = 0
    diagnostic_polls = 0
    stopped = "duration_elapsed"
    metadata = {
        "started_utc": started_utc,
        "device_url": args.url,
        "duration_s": args.duration,
        "status_interval_s": STATUS_INTERVAL,
        "diagnostic_interval_s": DIAGNOSTIC_INTERVAL,
        "request_timeout_s": args.timeout,
        "requests": "GET /api/status; GET /api/diagnostics/wifi; POST /api/login when required",
    }
    (output / "capture.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print("Saving diagnostics to " + str(output), flush=True)
    print("Polling status every 5 s and authenticated logs every 15 s; Ctrl+C stops safely.", flush=True)

    with (output / "status.ndjson").open("w", encoding="utf-8") as statuses, \
            (output / "wifi-diagnostics.ndjson").open("wb") as diagnostics, \
            (output / "collector.ndjson").open("w", encoding="utf-8") as observations:
        try:
            while time.monotonic() < deadline:
                now = time.monotonic()
                if now < next_status:
                    time.sleep(min(next_status - now, deadline - now, 0.25))
                    continue
                next_status = now + STATUS_INTERVAL
                captured = utc_now()
                code, payload, error = device.request("/api/status")
                status_polls += 1
                status = None
                if code == 200:
                    try:
                        status = json.loads(payload)
                        if not isinstance(status, dict):
                            raise ValueError("status is not an object")
                    except (ValueError, UnicodeError):
                        error = "invalid_status_json"
                healthy = code == 200 and status is not None
                recovered = healthy and evidence.open_outage is not None
                if healthy:
                    previous = evidence.last_status
                    rebooted = previous and numeric(status.get("uptime_s")) and numeric(previous.get("uptime_s")) and status["uptime_s"] < previous["uptime_s"]
                    evidence.observe_status(status, captured)
                    write_json_line(statuses, {"captured_utc": captured, "status": status})
                    wifi = status.get("wifi", {})
                    wifi_connected = wifi.get("connected") if isinstance(wifi, dict) else None
                    console_state = ("reachable", wifi_connected)
                    if console_state != last_console_state or recovered or rebooted:
                        print("{} HTTP reachable; Wi-Fi={}; uptime={} s{}".format(
                            captured, wifi_connected, status.get("uptime_s", "unknown"),
                            "; uptime decreased" if rebooted else ""), flush=True)
                    if recovered or rebooted:
                        next_diagnostic = 0
                else:
                    evidence.observe_failure(captured, error or "unexpected_status")
                    write_json_line(observations, {"captured_utc": captured, "kind": "status_failure", "http_status": code, "error": error})
                    console_state = ("unreachable", error)
                    if console_state != last_console_state:
                        print(captured + " status unavailable (" + str(error) + "); collection continues.", flush=True)
                last_console_state = console_state

                if healthy and time.monotonic() >= next_diagnostic:
                    next_diagnostic = time.monotonic() + DIAGNOSTIC_INTERVAL
                    log_code, raw, log_error = device.diagnostics()
                    diagnostic_polls += 1
                    fetched_utc = utc_now()
                    if log_code == 200 and raw is not None:
                        (output / "diagnostics-latest.ndjson").write_bytes(raw)
                        added = 0
                        for line in raw.splitlines():
                            if not line.strip() or line in seen:
                                continue
                            seen.add(line)
                            added += 1
                            evidence.unique_lines += 1
                            diagnostics.write(line + b"\n")
                            try:
                                record = json.loads(line)
                                if not isinstance(record, dict):
                                    raise ValueError("event is not an object")
                                evidence.observe_event(record)
                            except (ValueError, UnicodeError):
                                evidence.invalid_lines += 1
                        diagnostics.flush()
                        write_json_line(observations, {"captured_utc": fetched_utc, "kind": "diagnostic_fetch", "bytes": len(raw), "new_lines": added})
                        if last_diagnostic_error:
                            print(fetched_utc + " authenticated diagnostics available again.", flush=True)
                        last_diagnostic_error = None
                    else:
                        write_json_line(observations, {"captured_utc": fetched_utc, "kind": "diagnostic_failure", "http_status": log_code, "error": log_error})
                        if log_error != last_diagnostic_error:
                            print(fetched_utc + " diagnostics unavailable (" + str(log_error) + "); status collection continues.", flush=True)
                        last_diagnostic_error = log_error
        except KeyboardInterrupt:
            stopped = "interrupted"
            print("\nStopping collection.", flush=True)
        finally:
            summary = evidence.summary()
            summary.update({"started_utc": started_utc, "finished_utc": utc_now(), "stop_reason": stopped,
                            "status_polls": status_polls, "diagnostic_polls": diagnostic_polls})
            (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("Captured {} unique diagnostic lines; {} HTTP outage(s); {} observed uptime decrease(s).".format(
        evidence.unique_lines, len(evidence.outages), evidence.boot_changes), flush=True)
    print("Raw SDK statuses: {}; numeric disconnect reasons: {}.".format(
        dict(evidence.sdk_statuses), dict(evidence.reasons)), flush=True)
    outcomes = Counter(item["outcome"] for item in evidence.association_outcomes)
    print("Observed association outcomes: {}; still without a later GOT_IP/disconnect: {}.".format(
        dict(outcomes), len(evidence.pending_associations)), flush=True)
    print("Summary: " + str(output / "summary.json"), flush=True)
    if not evidence.unique_lines:
        print("No diagnostic event lines were downloaded; inspect collector.ndjson for authentication/API errors.", flush=True)
    print("HTTP outages alone do not identify the Wi-Fi failure cause.", flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://192.168.1.69", help="device origin (default: %(default)s)")
    parser.add_argument("--duration", type=float, default=600, help="collection duration in seconds; 0 runs until Ctrl+C (default: %(default)s)")
    parser.add_argument("--timeout", type=float, default=4, help="per-request socket timeout in seconds (default: %(default)s)")
    parser.add_argument("--password-env", metavar="NAME", help="read the portal password from this environment variable instead of prompting")
    args = parser.parse_args()
    origin = urllib.parse.urlsplit(args.url)
    if origin.scheme not in ("http", "https") or not origin.hostname or origin.username or origin.password or origin.path not in ("", "/") or origin.query or origin.fragment:
        parser.error("--url must be an HTTP(S) device origin without credentials, path, query, or fragment")
    args.url = args.url.rstrip("/")
    if not (0 <= args.duration < float("inf")) or not (0 < args.timeout <= 30):
        parser.error("--duration must be finite and nonnegative; --timeout must be between 0 and 30 seconds")
    if args.password_env:
        password = os.environ.get(args.password_env)
        if not password:
            parser.error("the password environment variable is missing or empty")
    else:
        if not sys.stdin.isatty():
            parser.error("use --password-env NAME for unattended collection")
        password = getpass.getpass("Portal password (not saved): ")
        if not password:
            parser.error("the portal password must not be empty")
    try:
        return capture(args, password)
    except OSError as exc:
        print("Cannot write capture files: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
