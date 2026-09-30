"use client";

// Архив (ТЗ §13): выбор даты → сегменты на таймлайне → просмотр (HLS) и скачивание MP4.
import HlsPlayer from "@/components/HlsPlayer";
import { apiJson } from "@/lib/api";
import { useAuth } from "@/lib/useAuth";
import Link from "next/link";
import { useEffect, useState } from "react";

interface Recording {
  id: number;
  started_at: string;
  ended_at?: string;
  file_size?: number;
}

export default function ArchivePage() {
  const { user, loading } = useAuth();
  const [date, setDate] = useState(() => new Date().toISOString().slice(0, 10));
  const [recordings, setRecordings] = useState<Recording[]>([]);
  const [playUrl, setPlayUrl] = useState("");
  const [exporting, setExporting] = useState(false);
  const [message, setMessage] = useState("");

  useEffect(() => {
    if (!user) return;
    setPlayUrl("");
    apiJson<{ recordings: Recording[] }>(`/api/recordings?date=${date}`)
      .then((r) => setRecordings(r.recordings))
      .catch(() => setRecordings([]));
  }, [user, date]);

  if (loading || !user) return null;

  async function play(rec: Recording) {
    setMessage("");
    try {
      // смотрим выбранный сегмент целиком (ТЗ §73.2 — HLS из сегментов)
      const res = await apiJson<{ url: string }>("/api/archive/session", {
        method: "POST",
        body: JSON.stringify({ start: rec.started_at, end: rec.ended_at }),
      });
      setPlayUrl(res.url);
    } catch (err) {
      setMessage(err instanceof Error ? err.message : "Ошибка");
    }
  }

  async function download(rec: Recording) {
    setExporting(true);
    setMessage("");
    try {
      const task = await apiJson<{ id: number }>("/api/export", {
        method: "POST",
        body: JSON.stringify({ start: rec.started_at, end: rec.ended_at }),
      });
      // ждём готовности файла
      for (let i = 0; i < 60; i++) {
        await new Promise((r) => setTimeout(r, 2000));
        const st = await apiJson<{ status: string; download_url?: string }>(
          `/api/export/${task.id}`
        );
        if (st.status === "READY" && st.download_url) {
          const a = document.createElement("a");
          a.href = st.download_url;
          a.download = "";
          a.click();
          return;
        }
        if (st.status === "FAILED") throw new Error("Не удалось подготовить файл");
      }
      throw new Error("Превышено время ожидания");
    } catch (err) {
      setMessage(err instanceof Error ? err.message : "Ошибка");
    } finally {
      setExporting(false);
    }
  }

  return (
    <main className="animate-page mx-auto flex min-h-screen w-full max-w-2xl flex-col gap-4 p-4 pb-10">
      <header className="flex items-center justify-between pt-2">
        <h1 className="text-2xl font-semibold">Архив</h1>
        <Link href="/" className="transition-soft text-neutral-500 active:opacity-60">Назад</Link>
      </header>

      <input
        type="date"
        value={date}
        onChange={(e) => setDate(e.target.value)}
        className="transition-soft rounded-xl border border-neutral-200 bg-white px-4 py-3"
      />

      {playUrl && <HlsPlayer src={playUrl} />}

      {message && <p className="animate-fade text-sm text-red-500">{message}</p>}

      {recordings.length === 0 ? (
        <p className="animate-fade text-neutral-400">За этот день записей нет</p>
      ) : (
        <ul className="flex flex-col gap-2">
          {recordings.map((r, i) => (
            <li key={r.id}
              style={{ animationDelay: `${Math.min(i, 10) * 50}ms` }}
              className="animate-page card-hover flex items-center justify-between gap-3 rounded-xl bg-white p-3 shadow-sm">
              <span className="font-medium">
                {new Date(r.started_at).toLocaleTimeString("ru-RU", {
                  hour: "2-digit", minute: "2-digit",
                })}
              </span>
              <div className="flex gap-2">
                <button
                  onClick={() => play(r)}
                  className="transition-soft rounded-lg bg-neutral-900 px-4 py-2 text-white hover:bg-neutral-700 active:scale-95 active:opacity-80"
                >
                  Смотреть
                </button>
                <button
                  onClick={() => download(r)}
                  disabled={exporting}
                  className="transition-soft rounded-lg bg-neutral-100 px-4 py-2 hover:bg-neutral-200 active:scale-95 active:opacity-70 disabled:opacity-40"
                >
                  {exporting ? "Подготовка…" : "Скачать"}
                </button>
              </div>
            </li>
          ))}
        </ul>
      )}
    </main>
  );
}
