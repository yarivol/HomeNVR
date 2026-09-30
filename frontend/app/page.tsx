"use client";

// Главная страница (ТЗ §11): live preview, статус камеры, последние события.
import HlsPlayer from "@/components/HlsPlayer";
import { apiJson } from "@/lib/api";
import { useAuth, clearAuthCache } from "@/lib/useAuth";
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

// Кэш последних данных между SPA-переходами: при возврате на главную
// показываем их мгновенно (без «прыжка» пустое→полное), затем тихо обновляем.
let cachedCamera: CameraInfo | null = null;
let cachedEvents: MotionEvent[] = [];
let firstMountDone = false;

export default function HomePage() {
  const { user, loading } = useAuth();
  const router = useRouter();
  const [camera, setCamera] = useState<CameraInfo | null>(cachedCamera);
  const [events, setEvents] = useState<MotionEvent[]>(cachedEvents);
  const [eventsLoaded, setEventsLoaded] = useState(cachedEvents.length > 0);
  // анимация появления — только при самом первом заходе, не при возвратах
  const [animate] = useState(!firstMountDone);

  useEffect(() => {
    if (!user) return;
    firstMountDone = true;
    apiJson<CameraInfo>("/api/camera")
      .then((c) => { cachedCamera = c; setCamera(c); })
      .catch(() => {});
    apiJson<{ events: MotionEvent[] }>("/api/events")
      .then((r) => {
        cachedEvents = r.events.slice(0, 5);
        setEvents(cachedEvents);
        setEventsLoaded(true);
      })
      .catch(() => setEventsLoaded(true));

    // realtime-обновления через WebSocket (ТЗ §41), с автореконнектом —
    // backend может перезапуститься, соединение надо поднимать заново
    let ws: WebSocket | null = null;
    let closed = false;
    let retry: ReturnType<typeof setTimeout> | null = null;
    const connect = () => {
      if (closed) return;
      const proto = location.protocol === "https:" ? "wss" : "ws";
      ws = new WebSocket(`${proto}://${location.host}/ws`);
      ws.onmessage = (msg) => {
        try {
          const { event } = JSON.parse(msg.data);
          if (event.startsWith("camera."))
            apiJson<CameraInfo>("/api/camera")
              .then((c) => { cachedCamera = c; setCamera(c); })
              .catch(() => {});
          if (event === "motion.ended")
            apiJson<{ events: MotionEvent[] }>("/api/events")
              .then((r) => {
                cachedEvents = r.events.slice(0, 5);
                setEvents(cachedEvents);
              })
              .catch(() => {});
        } catch {}
      };
      ws.onclose = () => {
        if (!closed) retry = setTimeout(connect, 5000);
      };
    };
    connect();
    return () => {
      closed = true;
      if (retry) clearTimeout(retry);
      ws?.close();
    };
  }, [user]);

  if (loading || !user) return null;

  async function logout() {
    clearAuthCache();
    await apiJson("/api/auth/logout", { method: "POST" }).catch(() => {});
    router.replace("/login");
  }

  return (
    <main className={(animate ? "animate-page " : "") + "mx-auto flex min-h-screen w-full max-w-2xl flex-col gap-6 p-4 pb-10"}>
      <header className="flex items-center justify-between pt-2">
        <h1 className="text-2xl font-semibold">{camera?.name ?? "Камера"}</h1>
        <button onClick={logout} className="text-neutral-500 active:opacity-60">
          Выйти
        </button>
      </header>

      <p
        className={
          "flex items-center gap-2 text-sm " +
          (camera?.status === "connected" ? "text-green-600" : "text-red-500")
        }
      >
        <span
          className={
            "inline-block h-2 w-2 rounded-full " +
            (camera?.status === "connected"
              ? "animate-pulse-soft bg-green-500"
              : "bg-red-500")
          }
        />
        {STATUS_TEXT[camera?.status ?? ""] ?? camera?.status}
      </p>

      {camera?.configured && camera.status === "connected" ? (
        <HlsPlayer src="/stream/live/index.m3u8" live />
      ) : (
        <div className="flex aspect-video items-center justify-center rounded-2xl bg-neutral-200 text-neutral-500">
          {camera?.configured ? (
            "Камера недоступна"
          ) : (
            "Камера не настроена"
          )}
        </div>
      )}

      <nav className="grid grid-cols-2 gap-3">
        <Link href="/archive"
          className="card-hover rounded-2xl bg-white py-4 text-center text-lg shadow-sm active:opacity-70">
          Архив
        </Link>
        <Link href="/events"
          className="card-hover rounded-2xl bg-white py-4 text-center text-lg shadow-sm active:opacity-70">
          События
        </Link>
        {user.role === "ADMIN" && (
          <Link href="/admin"
            className="card-hover col-span-2 rounded-2xl bg-neutral-900 py-4 text-center text-lg text-white active:opacity-80">
            Настройки
          </Link>
        )}
      </nav>

      <section>
        <h2 className="mb-2 text-lg font-medium">События</h2>
        {!eventsLoaded ? (
          // скелетоны фиксированной высоты — контент не прыгает при загрузке
          <ul className="flex flex-col gap-2">
            {[0, 1, 2].map((i) => (
              <li key={i} className="flex items-center gap-3 rounded-xl bg-white p-3 shadow-sm">
                <div className="h-12 w-20 animate-pulse-soft rounded-lg bg-neutral-200" />
                <div className="h-4 w-40 animate-pulse-soft rounded bg-neutral-200" />
              </li>
            ))}
          </ul>
        ) : events.length === 0 ? (
          <p className="text-neutral-400">Пока нет событий</p>
        ) : (
          <ul className="flex flex-col gap-2">
            {events.map((e, i) => (
              <li key={e.id}
                style={animate ? { animationDelay: `${i * 60}ms` } : undefined}
                className={(animate ? "animate-page " : "") + "card-hover flex items-center gap-3 rounded-xl bg-white p-3 shadow-sm"}>
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
