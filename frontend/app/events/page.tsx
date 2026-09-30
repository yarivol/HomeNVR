"use client";

// События движения (ТЗ §24): список с thumbnails.
import { apiJson } from "@/lib/api";
import { useAuth } from "@/lib/useAuth";
import Link from "next/link";
import { useEffect, useState } from "react";

interface MotionEvent {
  id: number;
  started_at: string;
  ended_at?: string;
  score?: number;
  thumbnail?: string;
}

export default function EventsPage() {
  const { user, loading } = useAuth();
  const [date, setDate] = useState(() => new Date().toISOString().slice(0, 10));
  const [events, setEvents] = useState<MotionEvent[]>([]);

  useEffect(() => {
    if (!user) return;
    apiJson<{ events: MotionEvent[] }>(`/api/events?date=${date}`)
      .then((r) => setEvents(r.events))
      .catch(() => setEvents([]));
  }, [user, date]);

  if (loading || !user) return null;

  return (
    <main className="animate-page mx-auto flex min-h-screen w-full max-w-2xl flex-col gap-4 p-4 pb-10">
      <header className="flex items-center justify-between pt-2">
        <h1 className="text-2xl font-semibold">События</h1>
        <Link href="/" className="transition-soft text-neutral-500 active:opacity-60">Назад</Link>
      </header>

      <input
        type="date"
        value={date}
        onChange={(e) => setDate(e.target.value)}
        className="transition-soft rounded-xl border border-neutral-200 bg-white px-4 py-3"
      />

      {events.length === 0 ? (
        <p className="animate-fade text-neutral-400">За этот день событий нет</p>
      ) : (
        <ul className="flex flex-col gap-2">
          {events.map((e, i) => (
            <li key={e.id}
              style={{ animationDelay: `${Math.min(i, 10) * 50}ms` }}
              className="animate-page card-hover flex items-center gap-3 rounded-xl bg-white p-3 shadow-sm">
              {e.thumbnail ? (
                // eslint-disable-next-line @next/next/no-img-element
                <img src={e.thumbnail} alt="" className="h-16 w-28 rounded-lg object-cover" />
              ) : (
                <div className="h-16 w-28 rounded-lg bg-neutral-200" />
              )}
              <div>
                <div className="font-medium">
                  {new Date(e.started_at).toLocaleTimeString("ru-RU")}
                </div>
                <div className="text-sm text-neutral-500">Обнаружено движение</div>
              </div>
              <Link
                href={`/archive?date=${date}&at=${encodeURIComponent(e.started_at)}`}
                className="transition-soft ml-auto rounded-lg bg-neutral-900 px-4 py-2 text-sm text-white hover:bg-neutral-700 active:scale-95"
              >
                Смотреть
              </Link>
            </li>
          ))}
        </ul>
      )}
    </main>
  );
}
