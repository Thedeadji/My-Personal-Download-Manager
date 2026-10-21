"""Time-based scheduling of queue actions.

Lets the download manager start/pause the queue (or change the global speed
limit) at set times — e.g. "download only between 01:00 and 07:00" or "start
everything at 15:30 today". A background thread wakes periodically and fires any
rule whose time the clock has crossed since the last check.

Firing is **edge-triggered**: a rule fires when the scheduled moment falls in the
open-closed interval (last_check, now]. That makes each occurrence fire exactly
once and tolerates a late/overslept tick, while never retroactively firing a time
that passed before the scheduler was running. If the app isn't running at the
scheduled time, that occurrence is simply missed (the manager must be alive to
act), which matches how a download manager's scheduler is expected to behave.

The Scheduler is UI-agnostic: it's handed an `actions` registry (name -> callable
taking the rule) so it has no dependency on the manager internals, which also
makes it trivial to unit-test with an injected clock.
"""
from __future__ import annotations

import datetime as _dt
import json
import threading
import time
import uuid
from dataclasses import dataclass, field, asdict
from typing import Callable, Optional

# Action names a rule may invoke; the manager supplies the implementations.
ACTIONS = ("pause_all", "resume_all", "set_global_rate")
REPEATS = ("daily", "once")


@dataclass
class ScheduleRule:
    id: str
    action: str                       # one of ACTIONS
    at: str                           # "HH:MM" (daily) or ISO "YYYY-MM-DDTHH:MM" (once)
    repeat: str = "daily"             # "daily" | "once"
    days: list = field(default_factory=list)   # weekday filter 0=Mon..6=Sun; [] = every day
    rate_bps: int = 0                 # payload for set_global_rate
    enabled: bool = True

    def to_dict(self) -> dict:
        return asdict(self)


def _parse_hm(at: str) -> tuple:
    h, m = at.split(":")
    h, m = int(h), int(m)
    if not (0 <= h < 24 and 0 <= m < 60):
        raise ValueError(f"invalid time of day: {at!r}")
    return h, m


def _parse_once(at: str) -> _dt.datetime:
    # Accept "YYYY-MM-DDTHH:MM" or with seconds; also tolerate a space separator.
    return _dt.datetime.fromisoformat(at.replace(" ", "T"))


def rule_fires_between(rule: ScheduleRule, prev: _dt.datetime, now: _dt.datetime) -> bool:
    """Did this rule's scheduled time fall in the interval (prev, now]?"""
    if not rule.enabled or prev >= now:
        return False
    if rule.repeat == "once":
        try:
            when = _parse_once(rule.at)
        except ValueError:
            return False
        return prev < when <= now
    # daily: check every calendar day the (prev, now] window touches.
    h, m = _parse_hm(rule.at)
    day = prev.date()
    last = now.date()
    while day <= last:
        when = _dt.datetime.combine(day, _dt.time(h, m))
        if prev < when <= now and (not rule.days or when.weekday() in rule.days):
            return True
        day += _dt.timedelta(days=1)
    return False


class Scheduler:
    def __init__(self, actions: dict, store=None, interval: float = 15.0):
        self.actions: dict[str, Callable[[ScheduleRule], None]] = actions
        self.rules: list[ScheduleRule] = []
        self._store = store
        self._interval = interval
        self._lock = threading.Lock()
        self._last_check: Optional[_dt.datetime] = None
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    # ---- persistence -----------------------------------------------------
    def load(self):
        if not self._store:
            return
        try:
            data = json.loads(open(self._store, encoding="utf-8").read())
        except (OSError, json.JSONDecodeError):
            return
        with self._lock:
            self.rules = [
                ScheduleRule(**{k: v for k, v in d.items()
                                if k in ScheduleRule.__dataclass_fields__})
                for d in data.get("rules", [])
            ]

    def save(self):
        if not self._store:
            return
        with self._lock:
            data = {"rules": [r.to_dict() for r in self.rules]}
        try:
            tmp = str(self._store) + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            import os
            os.replace(tmp, self._store)
        except OSError:
            pass

    # ---- rule management -------------------------------------------------
    def add_rule(self, action: str, at: str, repeat: str = "daily",
                 days: Optional[list] = None, rate_bps: int = 0,
                 enabled: bool = True) -> ScheduleRule:
        if action not in ACTIONS:
            raise ValueError(f"unknown action {action!r}; expected one of {ACTIONS}")
        if repeat not in REPEATS:
            raise ValueError(f"unknown repeat {repeat!r}; expected one of {REPEATS}")
        # Validate the time format up front so a bad rule can't silently never fire.
        _parse_once(at) if repeat == "once" else _parse_hm(at)
        rule = ScheduleRule(id=uuid.uuid4().hex[:12], action=action, at=at,
                            repeat=repeat, days=list(days or []),
                            rate_bps=max(0, int(rate_bps)), enabled=enabled)
        with self._lock:
            self.rules.append(rule)
        self.save()
        return rule

    def remove_rule(self, rule_id: str):
        with self._lock:
            self.rules = [r for r in self.rules if r.id != rule_id]
        self.save()

    def set_enabled(self, rule_id: str, enabled: bool):
        with self._lock:
            for r in self.rules:
                if r.id == rule_id:
                    r.enabled = enabled
        self.save()

    def list_rules(self) -> list:
        with self._lock:
            return list(self.rules)

    # ---- firing ----------------------------------------------------------
    def due(self, prev: _dt.datetime, now: _dt.datetime) -> list:
        with self._lock:
            rules = list(self.rules)
        return [r for r in rules if rule_fires_between(r, prev, now)]

    def tick(self, now: Optional[_dt.datetime] = None):
        """Fire any rules whose time passed since the previous tick."""
        now = now or _dt.datetime.now()
        prev = self._last_check
        self._last_check = now
        if prev is None:          # first tick establishes the baseline; no backfire
            return
        for rule in self.due(prev, now):
            fn = self.actions.get(rule.action)
            if fn is None:
                continue
            try:
                fn(rule)
            except Exception:     # a misbehaving action must not kill the loop
                pass
            if rule.repeat == "once":
                self.set_enabled(rule.id, False)

    # ---- background thread ----------------------------------------------
    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._last_check = _dt.datetime.now()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()

    def _loop(self):
        while not self._stop.wait(self._interval):
            self.tick()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
