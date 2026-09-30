"use client";

// Главная страница (ТЗ §11): live preview, статус камеры, последние события.
import HlsPlayer from "@/components/HlsPlayer";
import { apiJson } from "@/lib/api";
import { useAuth } from "@/lib/useAuth";
import Link from "next/link";
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";

interface CameraInfo {
  configured: boolean;
  name?: string;
  status: string;
}

interface MotionEvent {
  id: number;
  started_at: string;
  score?: number;
  thumbnail?: string;
}

const STATUS_TEXT: Record<string, string> = {
  connected: "Подключена",
  disconnected: "Камера недоступна",
  reconnecting: "Переподключение…",
};

export default function HomePage() {
  const { user, loading } = useAuth();
  const router = useRouter();
  const [camera, setCamera] = useState<CameraInfo | null>(null);
  const [events, setEvents] = useState<MotionEvent[]>([]);

  useEffect(() => {
    if (!user) return;
    apiJson<CameraInfo>("/api/camera").then(setCamera).catch(() => {});
    apiJson<{ events: MotionEvent[] }>("/api/events")
      .then((r) => setEvents(r.events.slice(0, 5)))
      .catch(() => {});

    // realtime-обновления через WebSocket (ТЗ §41)
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const ws = new WebSocket(`${proto}://${location.host}/ws`);
    ws.onmessage = (msg) => {
      try {
        const { event } = JSON.parse(msg.data);
        if (event.startsWith("camera."))
          apiJson<CameraInfo>("/api/camera").then(setCamera).catch(() => {});
        if (event === "motion.ended")
          apiJson<{ events: MotionEvent[] }>("/api/events")
            .then((r) => setEvents(r.events.slice(0, 5)))
            .catch(() => {});
      } catch {}
    };
    return () => ws.close();
  }, [user]);

  if (loading || !user) return null;

  async function logout() {
    await apiJson("/api/auth/logout", { method: "POST" }).catch(() => {});
    router.replace("/login");
  }

  return (
    <main className="mx-auto flex min-h-screen w-full max-w-2xl flex-col gap-6 p-4 pb-10">
      <header className="flex items-center justify-between pt-2">
        <h1 className="text-2xl font-semibold">{camera?.name ?? "Камера"}</h1>
        <button onClick={logout} className="text-neutral-500 active:opacity-60">
          Выйти
        </button>
      </header>

      <p
        className={
          "text-sm " +
          (camera?.status === "connected" ? "text-green-600" : "text-red-500")
        }
      >
        {STATUS_TEXT[camera?.status ?? ""] ?? camera?.status}
      </p>

      {camera?.configured && camera.status === "connected" ? (
        <HlsPlayer src="/stream/live/index.m3u8" live />
      ) : (
        <div className="flex aspect-video items-center justify-center rounded-2xl bg-neutral-200 text-neutral-500">
          {camera?.configured ? "Камера недоступна" : "Камера не настроена"}
        </div>
      )}

      <nav className="grid grid-cols-2 gap-3">
        <Link href="/archive"
          className="rounded-2xl bg-white py-4 text-center text-lg shadow-sm active:opacity-70">
          Архив
        </Link>
        <Link href="/events"
          className="rounded-2xl bg-white py-4 text-center text-lg shadow-sm active:opacity-70">
          События
        </Link>
        {user.role === "ADMIN" && (
          <Link href="/admin"
            className="col-span-2 rounded-2xl bg-neutral-900 py-4 text-center text-lg text-white active:opacity-80">
            Настройки
          </Link>
        )}
      </nav>

      <section>
        <h2 className="mb-2 text-lg font-medium">События</h2>
        {events.length === 0 ? (
          <p className="text-neutral-400">Пока нет событий</p>
        ) : (
          <ul className="flex flex-col gap-2">
            {events.map((e) => (
              <li key={e.id}
                className="flex items-center gap-3 rounded-xl bg-white p-3 shadow-sm">
                {e.thumbnail && (
                  // eslint-disable-next-line @next/next/no-img-element
                  <img src={e.thumbnail} alt="" className="h-12 w-20 rounded-lg object-cover" />
                )}
                <span>
                  {new Date(e.started_at).toLocaleTimeString("ru-RU", {
                    hour: "2-digit",
                    minute: "2-digit",
                  })}{" "}
                  Обнаружено движение
                </span>
              </li>
            ))}
          </ul>
        )}
      </section>
    </main>
  );
}
