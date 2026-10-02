"use client";

// Админ-панель (ТЗ §43-50): камера, запись, motion, хранилище, пользователи, система.
import { apiJson } from "@/lib/api";
import { useAuth } from "@/lib/useAuth";
import Link from "next/link";
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";

type Tab = "camera" | "video" | "recording" | "motion" | "storage" | "users" | "system" | "logs";

const TABS: { id: Tab; label: string }[] = [
  { id: "camera", label: "Камера" },
  { id: "video", label: "Видео" },
  { id: "recording", label: "Запись" },
  { id: "motion", label: "Движение" },
  { id: "storage", label: "Хранилище" },
  { id: "users", label: "Пользователи" },
  { id: "system", label: "Система" },
  { id: "logs", label: "Логи" },
];

const input =
  "w-full rounded-xl border border-neutral-200 px-4 py-3 outline-none focus:border-neutral-400";
const btn =
  "rounded-xl bg-neutral-900 px-6 py-3 text-white active:opacity-80 disabled:opacity-40";
const card = "animate-page flex flex-col gap-3 rounded-2xl bg-white p-5 shadow-sm";

export default function AdminPage() {
  const { user, loading } = useAuth();
  const router = useRouter();
  const [tab, setTab] = useState<Tab>("camera");

  useEffect(() => {
    if (!loading && user && user.role !== "ADMIN") router.replace("/");
  }, [user, loading, router]);

  if (loading || !user || user.role !== "ADMIN") return null;

  return (
    <main className="mx-auto flex min-h-screen w-full max-w-2xl flex-col gap-4 p-4 pb-10">
      <header className="flex items-center justify-between pt-2">
        <h1 className="text-2xl font-semibold">Настройки</h1>
        <Link href="/" className="text-neutral-500 active:opacity-60">Назад</Link>
      </header>

      <nav className="flex flex-wrap gap-2">
        {TABS.map((t) => (
          <button
            key={t.id}
            onClick={() => setTab(t.id)}
            className={
              "rounded-full px-4 py-2 text-sm active:opacity-70 " +
              (tab === t.id ? "bg-neutral-900 text-white" : "bg-white shadow-sm")
            }
          >
            {t.label}
          </button>
        ))}
      </nav>

      {tab === "camera" && <CameraTab />}
      {tab === "video" && <VideoTab />}
      {tab === "recording" && <RecordingTab />}
      {tab === "motion" && <MotionTab />}
      {tab === "storage" && <StorageTab />}
      {tab === "users" && <UsersTab />}
      {tab === "system" && <SystemTab />}
      {tab === "logs" && <LogsTab />}
    </main>
  );
}

/* ---------------- Камера (ТЗ §44) ---------------- */
function CameraTab() {
  const [form, setForm] = useState({
    name: "", ip: "", onvif_port: 80, username: "", password: "",
    rtsp_url: "", rtsp_sub_url: "",
  });
  const [msg, setMsg] = useState("");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    apiJson<{ name?: string; ip_address?: string; onvif_port?: number; username?: string }>(
      "/api/camera"
    )
      .then((c) =>
        setForm((f) => ({
          ...f,
          name: c.name ?? "", ip: c.ip_address ?? "",
          onvif_port: c.onvif_port ?? 80, username: c.username ?? "",
        }))
      )
      .catch(() => {});
  }, []);

  const set = (k: string, v: string | number) => setForm((f) => ({ ...f, [k]: v }));

  async function save() {
    setBusy(true);
    setMsg("");
    try {
      const body: Record<string, unknown> = {
        name: form.name, ip: form.ip, onvif_port: form.onvif_port, username: form.username,
      };
      if (form.password) body.password = form.password;
      if (form.rtsp_url) body.rtsp_url = form.rtsp_url;
      if (form.rtsp_sub_url) body.rtsp_sub_url = form.rtsp_sub_url;
      await apiJson("/api/camera", { method: "PATCH", body: JSON.stringify(body) });
      setMsg("Сохранено ✓");
    } catch (e) {
      setMsg(e instanceof Error ? e.message : "Ошибка");
    } finally {
      setBusy(false);
    }
  }

  async function test() {
    setBusy(true);
    setMsg("Проверка…");
    try {
      const r = await apiJson<{ onvif?: { ok: boolean; model?: string }; rtsp?: { ok: boolean } }>(
        "/api/camera/test",
        {
          method: "POST",
          body: JSON.stringify({
            ip: form.ip, onvif_port: form.onvif_port,
            username: form.username, password: form.password, rtsp_url: form.rtsp_url,
          }),
        }
      );
      const parts: string[] = [];
      parts.push(r.onvif?.ok ? `ONVIF ✓ (${r.onvif.model ?? "ок"})` : "ONVIF ✗");
      if (r.rtsp) parts.push(r.rtsp.ok ? "RTSP ✓" : "RTSP ✗");
      setMsg(parts.join(" · "));
    } catch {
      setMsg("Проверка не удалась");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={card}>
      <input className={input} placeholder="Название" value={form.name} onChange={(e) => set("name", e.target.value)} />
      <input className={input} placeholder="IP-адрес" value={form.ip} onChange={(e) => set("ip", e.target.value)} />
      <input className={input} type="number" placeholder="ONVIF порт" value={form.onvif_port}
        onChange={(e) => set("onvif_port", Number(e.target.value))} />
      <input className={input} placeholder="Логин камеры" value={form.username} onChange={(e) => set("username", e.target.value)} />
      <input className={input} type="password" placeholder="Пароль камеры (пусто — не менять)"
        value={form.password} onChange={(e) => set("password", e.target.value)} />
      <input className={input} placeholder="RTSP URL (основной)" value={form.rtsp_url} onChange={(e) => set("rtsp_url", e.target.value)} />
      <input className={input} placeholder="RTSP URL (суб-поток)" value={form.rtsp_sub_url} onChange={(e) => set("rtsp_sub_url", e.target.value)} />
      {msg && <p className="text-sm text-neutral-600">{msg}</p>}
      <div className="flex gap-2">
        <button className={btn} disabled={busy} onClick={save}>Сохранить</button>
        <button className="rounded-xl bg-neutral-100 px-6 py-3 active:opacity-70 disabled:opacity-40"
          disabled={busy} onClick={test}>Проверить</button>
      </div>
    </div>
  );
}

/* ---------------- Видео (ТЗ §45): параметры через ONVIF ---------------- */
type VideoConfig = { encoding: string; width: number; height: number; fps: number; bitrate_kbps: number };
type VideoInfo = {
  onvif: boolean;
  error?: string;
  profile?: string;
  config?: VideoConfig;
  options?: {
    resolutions?: { width: number; height: number }[];
    fps_min?: number; fps_max?: number;
    bitrate_min?: number; bitrate_max?: number;
  };
};

function VideoTab() {
  const [info, setInfo] = useState<VideoInfo | null>(null);
  const [loadErr, setLoadErr] = useState("");
  const [res, setRes] = useState("");        // "WxH"
  const [fps, setFps] = useState(0);
  const [bitrate, setBitrate] = useState(0);
  const [msg, setMsg] = useState("");
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    apiJson<VideoInfo>("/api/camera/video")
      .then((r) => {
        setInfo(r);
        if (r.config) {
          setRes(`${r.config.width}x${r.config.height}`);
          setFps(r.config.fps);
          setBitrate(r.config.bitrate_kbps);
        }
      })
      .catch((e) => setLoadErr(e instanceof Error ? e.message : "Ошибка загрузки"));
  }, []);

  async function save() {
    setBusy(true);
    setMsg("");
    try {
      const [w, h] = res.split("x").map(Number);
      const r = await apiJson<{ config: VideoConfig }>("/api/camera/video", {
        method: "PATCH",
        body: JSON.stringify({ width: w, height: h, fps, bitrate_kbps: bitrate }),
      });
      setMsg(`Сохранено ✓ Камера: ${r.config.width}x${r.config.height}, ${r.config.fps} fps, ${r.config.bitrate_kbps} кбит/с`);
    } catch (e) {
      setMsg(e instanceof Error ? e.message : "Ошибка");
    } finally {
      setBusy(false);
    }
  }

  if (loadErr) return <div className={card}><p className="text-sm text-red-600">{loadErr}</p></div>;
  if (!info) return <div className={card}><p className="text-neutral-400">Загрузка…</p></div>;

  if (!info.onvif)
    return (
      <div className={card}>
        <p className="text-neutral-600">
          {info.error ?? "Камера не отвечает по ONVIF — удалённая настройка видео недоступна."}
        </p>
        <p className="text-sm text-neutral-400">
          Параметры видео можно изменить в веб-интерфейсе самой камеры.
        </p>
      </div>
    );

  const c = info.config!;
  const opts = info.options;

  return (
    <div className={card}>
      <p className="text-sm text-neutral-500">
        Профиль «{info.profile}», кодек {c.encoding}
      </p>

      <label className="text-sm text-neutral-600">Разрешение</label>
      {opts?.resolutions?.length ? (
        <select className={input} value={res} onChange={(e) => setRes(e.target.value)}>
          {!opts.resolutions.some((r) => `${r.width}x${r.height}` === res) && res && (
            <option value={res}>{res} (текущее)</option>
          )}
          {opts.resolutions.map((r) => (
            <option key={`${r.width}x${r.height}`} value={`${r.width}x${r.height}`}>
              {r.width}x{r.height}
            </option>
          ))}
        </select>
      ) : (
        <input className={input} value={res} onChange={(e) => setRes(e.target.value)} placeholder="1920x1080" />
      )}

      <label className="text-sm text-neutral-600">
        FPS {opts?.fps_max ? `(допустимо ${opts.fps_min}–${opts.fps_max})` : ""}
      </label>
      <input className={input} type="number" value={fps} onChange={(e) => setFps(Number(e.target.value))} />

      <label className="text-sm text-neutral-600">
        Битрейт, кбит/с {opts?.bitrate_max ? `(допустимо ${opts.bitrate_min}–${opts.bitrate_max})` : ""}
      </label>
      <input className={input} type="number" value={bitrate} onChange={(e) => setBitrate(Number(e.target.value))} />

      {msg && <p className="text-sm text-neutral-600">{msg}</p>}
      <button className={btn} disabled={busy} onClick={save}>
        {busy ? "Применение…" : "Применить на камере"}
      </button>
      <p className="text-xs text-neutral-400">
        Изменение применяется к самой камере; запись и live переподключатся автоматически.
      </p>
    </div>
  );
}

/* ---------------- Запись (ТЗ §46) ---------------- */
function RecordingTab() {
  const [mode, setMode] = useState("continuous");
  const [segSec, setSegSec] = useState(300);
  const [preBuf, setPreBuf] = useState(10);
  const [msg, setMsg] = useState("");

  useEffect(() => {
    apiJson<Record<string, unknown>>("/api/admin/settings").then((s) => {
      setMode(String(s.recording_mode ?? "continuous"));
      setSegSec(Number(s.segment_duration_sec ?? 300));
      setPreBuf(Number(s.pre_buffer_sec ?? 10));
    }).catch(() => {});
  }, []);

  async function save() {
    setMsg("");
    try {
      await apiJson("/api/admin/settings", {
        method: "PATCH",
        body: JSON.stringify({
          recording_mode: mode,
          segment_duration_sec: segSec,
          pre_buffer_sec: preBuf,
        }),
      });
      setMsg("Сохранено ✓");
    } catch (e) {
      setMsg(e instanceof Error ? e.message : "Ошибка сохранения");
    }
  }

  return (
    <div className={card}>
      <label className="text-sm text-neutral-500">Режим записи</label>
      <select className={input} value={mode} onChange={(e) => setMode(e.target.value)}>
        <option value="continuous">Постоянная запись</option>
        <option value="motion">По движению</option>
      </select>
      <label className="text-sm text-neutral-500">Длительность сегмента (сек)</label>
      <input className={input} type="number" min={60} max={3600} value={segSec}
        onChange={(e) => setSegSec(Number(e.target.value))} />
      <label className="text-sm text-neutral-500">
        Pre-buffer — запас до начала движения (сек)
      </label>
      <input className={input} type="number" min={0} max={60} value={preBuf}
        onChange={(e) => setPreBuf(Number(e.target.value))} />
      <p className="text-xs text-neutral-400">
        Используется только в режиме «По движению»: событие записывается с запасом
        до его начала.
      </p>
      {msg && <p className="text-sm text-neutral-600">{msg}</p>}
      <button className={btn} onClick={save}>Сохранить</button>
    </div>
  );
}

/* ---------------- Motion (ТЗ §47) ---------------- */
function MotionTab() {
  const [enabled, setEnabled] = useState(true);
  const [sens, setSens] = useState(0.5);
  const [minEvent, setMinEvent] = useState(3);
  const [cooldown, setCooldown] = useState(10);
  const [msg, setMsg] = useState("");

  useEffect(() => {
    apiJson<Record<string, unknown>>("/api/admin/settings").then((s) => {
      setEnabled(Boolean(s.motion_enabled ?? true));
      setSens(Number(s.motion_sensitivity ?? 0.5));
      setMinEvent(Number(s.motion_min_event_sec ?? 3));
      setCooldown(Number(s.motion_cooldown_sec ?? 10));
    }).catch(() => {});
  }, []);

  async function save() {
    try {
      await apiJson("/api/admin/settings", {
        method: "PATCH",
        body: JSON.stringify({
          motion_enabled: enabled, motion_sensitivity: sens,
          motion_min_event_sec: minEvent, motion_cooldown_sec: cooldown,
        }),
      });
      setMsg("Сохранено ✓");
    } catch {
      setMsg("Ошибка сохранения");
    }
  }

  return (
    <div className={card}>
      <label className="flex items-center justify-between">
        <span>Детекция движения</span>
        <input type="checkbox" className="h-6 w-6" checked={enabled}
          onChange={(e) => setEnabled(e.target.checked)} />
      </label>
      <label className="text-sm text-neutral-500">Чувствительность: {Math.round(sens * 100)}%</label>
      <input type="range" min={0} max={1} step={0.05} value={sens}
        onChange={(e) => setSens(Number(e.target.value))} />
      <label className="text-sm text-neutral-500">Минимальная длительность события (сек)</label>
      <input className={input} type="number" min={1} value={minEvent}
        onChange={(e) => setMinEvent(Number(e.target.value))} />
      <label className="text-sm text-neutral-500">Пауза между событиями (сек)</label>
      <input className={input} type="number" min={0} value={cooldown}
        onChange={(e) => setCooldown(Number(e.target.value))} />
      {msg && <p className="text-sm text-neutral-600">{msg}</p>}
      <button className={btn} onClick={save}>Сохранить</button>
    </div>
  );
}

/* ---------------- Хранилище (ТЗ §48) ---------------- */
function StorageTab() {
  const [stats, setStats] = useState<Record<string, number | string> | null>(null);
  const [maxUsage, setMaxUsage] = useState(90);
  const [minFree, setMinFree] = useState(10);
  const [msg, setMsg] = useState("");

  const loadStats = () =>
    apiJson<Record<string, number | string>>("/api/storage")
      .then(setStats)
      .catch(() => {});

  useEffect(() => {
    loadStats();
    apiJson<Record<string, unknown>>("/api/admin/settings").then((s) => {
      setMaxUsage(Math.round(Number(s.max_storage_usage ?? 0.9) * 100));
      setMinFree(Math.round(Number(s.min_free_space ?? 0.1) * 100));
    }).catch(() => {});
  }, []);

  async function save() {
    setMsg("");
    try {
      await apiJson("/api/admin/settings", {
        method: "PATCH",
        body: JSON.stringify({
          max_storage_usage: maxUsage / 100,
          min_free_space: minFree / 100,
        }),
      });
      setMsg("Сохранено ✓");
    } catch (e) {
      setMsg(e instanceof Error ? e.message : "Ошибка сохранения");
    }
  }

  const gb = (b?: number | string) =>
    b === undefined || b === null ? "—" : (Number(b) / 1024 ** 3).toFixed(1) + " ГБ";

  // M5: диск может быть заполнен данными, не относящимися к архиву —
  // тогда чистка удалит записи «впустую», это надо показывать админу
  const nonArchive = Number(stats?.non_archive_bytes ?? 0);
  const total = Number(stats?.total_bytes ?? 0);
  const nonArchiveHigh = total > 0 && nonArchive / total > 0.15;

  return (
    <div className={card}>
      {!stats ? (
        <p className="text-neutral-400">Загрузка…</p>
      ) : (
        <>
          <Row label="Занято на диске" value={`${stats.usage_percent}%`} />
          <Row label="Всего" value={gb(stats.total_bytes)} />
          <Row label="Свободно" value={gb(stats.free_bytes)} />
          <Row label="Размер архива" value={gb(stats.archive_bytes)} />
          {nonArchiveHigh && (
            <p className="text-sm text-red-600">
              ⚠ {gb(nonArchive)} занято данными вне архива записей (система, база,
              другие файлы). При заполнении диска старые записи будут удаляться,
              но место это не освободит — проверьте, что занимает диск.
            </p>
          )}
          <hr />
          <label className="text-sm text-neutral-500">
            Максимальное заполнение диска архивом (%)
          </label>
          <input className={input} type="number" min={50} max={95} value={maxUsage}
            onChange={(e) => setMaxUsage(Number(e.target.value))} />
          <label className="text-sm text-neutral-500">Минимальный свободный объём (%)</label>
          <input className={input} type="number" min={5} max={50} value={minFree}
            onChange={(e) => setMinFree(Number(e.target.value))} />
          {msg && <p className="text-sm text-neutral-600">{msg}</p>}
          <button className={btn} onClick={save}>Сохранить</button>
          <p className="text-sm text-neutral-500">
            При заполнении диска старые записи удаляются автоматически — запись не останавливается.
          </p>
        </>
      )}
    </div>
  );
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex justify-between">
      <span className="text-neutral-500">{label}</span>
      <span className="font-medium">{value}</span>
    </div>
  );
}

/* ---------------- Пользователи (ТЗ §49) ---------------- */
interface UserRow { id: number; username: string; role: string; enabled: boolean }

function UsersTab() {
  const [users, setUsers] = useState<UserRow[]>([]);
  const [newName, setNewName] = useState("");
  const [newPass, setNewPass] = useState("");
  const [newRole, setNewRole] = useState("USER");
  const [msg, setMsg] = useState("");

  function load() {
    apiJson<{ users: UserRow[] }>("/api/admin/users").then((r) => setUsers(r.users)).catch(() => {});
  }
  useEffect(load, []);

  async function create() {
    setMsg("");
    try {
      await apiJson("/api/admin/users", {
        method: "POST",
        body: JSON.stringify({ username: newName, password: newPass, role: newRole }),
      });
      setNewName(""); setNewPass("");
      load();
    } catch (e) {
      setMsg(e instanceof Error ? e.message : "Ошибка");
    }
  }

  async function toggle(u: UserRow) {
    await apiJson(`/api/admin/users/${u.id}`, {
      method: "PATCH", body: JSON.stringify({ enabled: !u.enabled }),
    }).catch(() => {});
    load();
  }

  async function remove(u: UserRow) {
    if (!confirm(`Удалить пользователя ${u.username}?`)) return;
    await apiJson(`/api/admin/users/${u.id}`, { method: "DELETE" }).catch((e) => setMsg(e.message));
    load();
  }

  return (
    <div className={card}>
      <ul className="flex flex-col gap-2">
        {users.map((u) => (
          <li key={u.id} className="flex items-center justify-between rounded-xl bg-neutral-50 p-3">
            <span>
              {u.username} <span className="text-sm text-neutral-400">({u.role})</span>
              {!u.enabled && <span className="ml-2 text-sm text-red-500">отключён</span>}
            </span>
            <span className="flex gap-2">
              <button className="text-sm text-neutral-600" onClick={() => toggle(u)}>
                {u.enabled ? "Отключить" : "Включить"}
              </button>
              <button className="text-sm text-red-500" onClick={() => remove(u)}>Удалить</button>
            </span>
          </li>
        ))}
      </ul>

      <hr />
      <input className={input} placeholder="Новый пользователь" value={newName}
        onChange={(e) => setNewName(e.target.value)} />
      <input className={input} type="password" placeholder="Пароль (минимум 8 символов)" value={newPass}
        onChange={(e) => setNewPass(e.target.value)} />
      <select className={input} value={newRole} onChange={(e) => setNewRole(e.target.value)}>
        <option value="USER">Пользователь</option>
        <option value="ADMIN">Администратор</option>
      </select>
      {msg && <p className="text-sm text-red-500">{msg}</p>}
      <button className={btn} disabled={!newName || newPass.length < 8} onClick={create}>
        Создать
      </button>
    </div>
  );
}

/* ---------------- Система (ТЗ §50) ---------------- */
// Русские подписи статусов — backend отдаёт машинные значения
const STATUS_RU: Record<string, Record<string, string>> = {
  camera: { connected: "Подключена", disconnected: "Недоступна", reconnecting: "Переподключение…" },
  recording: { running: "Идёт запись", stopped: "Остановлена", error: "Ошибка" },
  motion: { enabled: "Включена", disabled: "Выключена" },
  database: { ok: "ОК", error: "Ошибка" },
  backend: { ok: "ОК", error: "Ошибка" },
};

function SystemTab() {
  const [status, setStatus] = useState<Record<string, string | number> | null>(null);

  useEffect(() => {
    apiJson<Record<string, string | number>>("/api/system/status").then(setStatus).catch(() => {});
    const t = setInterval(() => {
      apiJson<Record<string, string | number>>("/api/system/status").then(setStatus).catch(() => {});
    }, 10000);
    return () => clearInterval(t);
  }, []);

  if (!status) return <div className={card}><p className="text-neutral-400">Загрузка…</p></div>;

  const ru = (key: string) => STATUS_RU[key]?.[String(status[key])] ?? String(status[key]);

  // русская плюрализация: 1 день / 2 дня / 5 дней
  const plural = (n: number, one: string, few: string, many: string) => {
    const m10 = n % 10, m100 = n % 100;
    if (m10 === 1 && m100 !== 11) return one;
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return few;
    return many;
  };
  const formatUptime = (sec: number) => {
    const d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600), m = Math.floor((sec % 3600) / 60);
    const parts: string[] = [];
    if (d) parts.push(`${d} ${plural(d, "день", "дня", "дней")}`);
    if (h) parts.push(`${h} ${plural(h, "час", "часа", "часов")}`);
    if (m || parts.length === 0) parts.push(`${m} ${plural(m, "минута", "минуты", "минут")}`);
    return parts.join(" ");
  };

  return (
    <div className={card}>
      <Row label="Камера" value={ru("camera")} />
      <Row label="Запись" value={ru("recording")} />
      <Row label="Детекция движения" value={ru("motion")} />
      <Row label="Диск занят" value={`${status.storage_percent}%`} />
      <Row label="База данных" value={ru("database")} />
      <Row label="Backend" value={ru("backend")} />
      {typeof status.uptime_sec === "number" && (
        <Row label="Аптайм" value={formatUptime(status.uptime_sec)} />
      )}
    </div>
  );
}

/* ---------------- Логи (просмотр backend.log) ---------------- */
const LOG_LEVELS = ["все", "info", "warning", "error", "debug"] as const;

function LogsTab() {
  const [lines, setLines] = useState<string[]>([]);
  const [filter, setFilter] = useState<(typeof LOG_LEVELS)[number]>("все");
  const [auto, setAuto] = useState(true);
  const [error, setError] = useState("");

  useEffect(() => {
    let alive = true;
    const load = () =>
      apiJson<{ lines: string[] }>("/api/admin/logs?lines=500")
        .then((r) => { if (alive) { setLines(r.lines); setError(""); } })
        .catch((e) => { if (alive) setError(e.message); });
    load();
    if (!auto) return () => { alive = false; };
    const t = setInterval(load, 5000);
    return () => { alive = false; clearInterval(t); };
  }, [auto]);

  // фильтр по уровню: строка вида "[...] [info] сообщение"
  const shown = filter === "все"
    ? lines
    : lines.filter((l) => l.includes(`[${filter}]`));

  return (
    <div className={card}>
      <div className="flex flex-wrap items-center gap-3">
        <select
          value={filter}
          onChange={(e) => setFilter(e.target.value as typeof filter)}
          className="rounded-xl border border-neutral-200 px-3 py-2"
        >
          {LOG_LEVELS.map((l) => (
            <option key={l} value={l}>{l === "все" ? "Все уровни" : l}</option>
          ))}
        </select>
        <label className="flex items-center gap-2 text-sm">
          <input type="checkbox" checked={auto} onChange={(e) => setAuto(e.target.checked)} />
          Обновлять каждые 5 сек
        </label>
        <span className="text-sm text-neutral-400">{shown.length} строк</span>
      </div>
      {error && <p className="text-sm text-red-600">{error}</p>}
      <pre className="max-h-[60vh] overflow-auto rounded-xl bg-neutral-900 p-3 text-xs leading-5 text-neutral-100">
        {shown.length ? shown.join("\n") : "Лог пуст"}
      </pre>
    </div>
  );
}
