"use client";

// Мастер первого запуска (ТЗ §73.5):
// шаг 1 — создать администратора, шаг 2 — камера (опционально), шаг 3 — готово.
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";

export default function SetupPage() {
  const router = useRouter();
  const [step, setStep] = useState(1);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");

  const [camName, setCamName] = useState("Камера");
  const [ip, setIp] = useState("");
  const [onvifPort, setOnvifPort] = useState("80");
  const [camUser, setCamUser] = useState("");
  const [camPass, setCamPass] = useState("");
  const [rtspUrl, setRtspUrl] = useState("");
  const [rtspSubUrl, setRtspSubUrl] = useState("");

  // setup уже завершён? — на главную
  useEffect(() => {
    fetch("/api/setup/status")
      .then((r) => r.json())
      .then((s) => s.completed && router.replace("/"))
      .catch(() => {});
  }, [router]);

  async function finish(skipCamera: boolean) {
    setBusy(true);
    setError("");
    try {
      const body: Record<string, unknown> = { username, password };
      if (!skipCamera && ip) {
        body.camera = {
          name: camName,
          ip,
          onvif_port: Number(onvifPort) || 80,
          username: camUser,
          password: camPass,
          rtsp_url: rtspUrl,
          rtsp_sub_url: rtspSubUrl,
        };
      }
      const res = await fetch("/api/setup", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (!res.ok) {
        const b = await res.json().catch(() => ({}));
        throw new Error(b.error ?? "Ошибка настройки");
      }
      setStep(3);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Ошибка");
    } finally {
      setBusy(false);
    }
  }

  const input =
    "rounded-xl border border-neutral-200 px-4 py-3 outline-none focus:border-neutral-400";
  const btn =
    "rounded-xl bg-neutral-900 py-3 text-lg text-white active:opacity-80 disabled:opacity-40";
  const btnGhost =
    "rounded-xl bg-white py-3 text-lg shadow-sm active:opacity-80 disabled:opacity-40";

  return (
    <main className="flex min-h-screen items-center justify-center p-6">
      <div className="animate-page flex w-full max-w-md flex-col gap-4 rounded-3xl bg-white p-8 shadow-sm">
        {step === 1 && (
          <>
            <h1 className="text-2xl font-semibold">Добро пожаловать!</h1>
            <p className="text-neutral-500">Создайте учётную запись администратора.</p>
            <input className={input} placeholder="Имя пользователя" value={username}
              onChange={(e) => setUsername(e.target.value)} />
            <input className={input} type="password" placeholder="Пароль (минимум 8 символов)"
              value={password} onChange={(e) => setPassword(e.target.value)} />
            {error && <p className="text-sm text-red-500">{error}</p>}
            <button className={btn} disabled={busy || !username || password.length < 8}
              onClick={() => setStep(2)}>
              Далее
            </button>
          </>
        )}

        {step === 2 && (
          <>
            <h1 className="text-2xl font-semibold">Камера</h1>
            <p className="text-neutral-500">Можно настроить сейчас или позже в настройках.</p>
            <input className={input} placeholder="Название" value={camName}
              onChange={(e) => setCamName(e.target.value)} />
            <input className={input} placeholder="IP-адрес камеры" value={ip}
              onChange={(e) => setIp(e.target.value)} />
            <div className="flex gap-3">
              <input className={input + " w-28"} placeholder="ONVIF порт" value={onvifPort}
                onChange={(e) => setOnvifPort(e.target.value)} />
              <input className={input + " flex-1"} placeholder="Логин камеры" value={camUser}
                onChange={(e) => setCamUser(e.target.value)} />
            </div>
            <input className={input} type="password" placeholder="Пароль камеры" value={camPass}
              onChange={(e) => setCamPass(e.target.value)} />
            <input className={input} placeholder="RTSP URL (основной поток)" value={rtspUrl}
              onChange={(e) => setRtspUrl(e.target.value)} />
            <input className={input} placeholder="RTSP URL (суб-поток, опционально)" value={rtspSubUrl}
              onChange={(e) => setRtspSubUrl(e.target.value)} />
            {error && <p className="text-sm text-red-500">{error}</p>}
            <button className={btn} disabled={busy} onClick={() => finish(false)}>
              Завершить настройку
            </button>
            <button className={btnGhost} disabled={busy} onClick={() => finish(true)}>
              Настрою позже
            </button>
          </>
        )}

        {step === 3 && (
          <>
            <h1 className="text-2xl font-semibold">Готово 🎉</h1>
            <p className="text-neutral-500">Система настроена. Войдите с новой учётной записью.</p>
            <button className={btn} onClick={() => router.replace("/login")}>
              Перейти ко входу
            </button>
          </>
        )}
      </div>
    </main>
  );
}
