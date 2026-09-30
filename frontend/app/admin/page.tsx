"use client";

// Админ-панель (ТЗ §43-50): камера, запись, motion, хранилище, пользователи, система.
import { apiJson } from "@/lib/api";
import { useAuth } from "@/lib/useAuth";
import Link from "next/link";
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";

type Tab = "camera" | "recording" | "motion" | "storage" | "users" | "system";

const TABS: { id: Tab; label: string }[] = [
  { id: "camera", label: "Камера" },
  { id: "recording", label: "Запись" },
  { id: "motion", label: "Движение" },
  { id: "storage", label: "Хранилище" },
  { id: "users", label: "Пользователи" },
  { id: "system", label: "Система" },
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
      {tab === "recording" && <RecordingTab />}
      {tab === "motion" && <MotionTab />}
      {tab === "storage" && <StorageTab />}
      {tab === "users" && <UsersTab />}
      {tab === "system" && <SystemTab />}
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

/* ---------------- Запись (ТЗ §46) ---------------- */
function RecordingTab() {
  const [mode, setMode] = useState("continuous");
  const [segSec, setSegSec] = useState(300);
  const [msg, setMsg] = useState("");

  useEffect(() => {
    apiJson<Record<string, unknown>>("/api/admin/settings").then((s) => {
      setMode(String(s.recording_mode ?? "continuous"));
      setSegSec(Number(s.segment_duration_sec ?? 300));
    }).catch(() => {});
  }, []);

  async function save() {
    try {
      await apiJson("/api/admin/settings", {
        method: "PATCH",
        body: JSON.stringify({ recording_mode: mode, segment_duration_sec: segSec }),
      });
      setMsg("Сохранено ✓");
    } catch {
      setMsg("Ошибка сохранения");
    }
  }

  return (
    <div className={card}>
      <label className="text-sm text-neutral-500">Режим записи</label>
      <select className={input} value={mode} onChange={(e) => setMode(e.target.value)}>
        <option value="continuous">Постоянная запись</option>
        <option value="motion">По движению (скоро)</option>
      </select>
      <label className="text-sm text-neutral-500">Длительность сегмента (сек)</label>
      <input className={input} type="number" min={60} max={600} value={segSec}
        onChange={(e) => setSegSec(Number(e.target.value))} />
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

  useEffect(() => {
    apiJson<Record<string, number | string>>("/api/storage").then(setStats).catch(() => {});
  }, []);

  const gb = (b?: number) => (b === undefined ? "—" : (b / 1024 ** 3).toFixed(1) + " ГБ");

  return (
    <div className={card}>
      {!stats ? (
        <p className="text-neutral-400">Загрузка…</p>
      ) : (
        <>
          <Row label="Занято на диске" value={`${stats.usage_percent}%`} />
          <Row label="Всего" value={gb(stats.total_bytes as number)} />
          <Row label="Свободно" value={gb(stats.free_bytes as number)} />
          <Row label="Размер архива" value={gb(stats.archive_bytes as number)} />
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

  return (
    <div className={card}>
      <Row label="Камера" value={String(status.camera)} />
      <Row label="Запись" value={String(status.recording)} />
      <Row label="Детекция движения" value={String(status.motion)} />
      <Row label="Диск занят" value={`${status.storage_percent}%`} />
      <Row label="База данных" value={String(status.database)} />
      <Row label="Backend" value={String(status.backend)} />
    </div>
  );
}
