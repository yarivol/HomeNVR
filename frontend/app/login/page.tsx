"use client";

// Страница входа (ТЗ §42)
import { apiJson } from "@/lib/api";
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";

export default function LoginPage() {
  const router = useRouter();
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  // если setup не завершён — туда и дорога
  useEffect(() => {
    fetch("/api/setup/status")
      .then((r) => r.json())
      .then((s) => !s.completed && router.replace("/setup"))
      .catch(() => {});
  }, [router]);

  async function onSubmit(e: React.FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError("");
    try {
      await apiJson("/api/auth/login", {
        method: "POST",
        body: JSON.stringify({ username, password }),
      });
      router.replace("/");
    } catch (err) {
      setError(err instanceof Error ? err.message : "Ошибка входа");
    } finally {
      setBusy(false);
    }
  }

  return (
    <main className="flex min-h-screen items-center justify-center p-6">
      <form
        onSubmit={onSubmit}
        className="animate-page flex w-full max-w-sm flex-col gap-4 rounded-3xl bg-white p-8 shadow-sm"
      >
        <div className="flex flex-col items-center gap-2">
          {/* eslint-disable-next-line @next/next/no-img-element */}
          <img src="/icon.svg" alt="HomeNVR" className="h-14 w-14" />
          <h1 className="text-center text-2xl font-semibold">HomeNVR</h1>
        </div>

        <input
          className="transition-soft rounded-xl border border-neutral-200 px-4 py-3 outline-none focus:border-neutral-400 focus:shadow-sm"
          placeholder="Имя пользователя"
          autoComplete="username"
          value={username}
          onChange={(e) => setUsername(e.target.value)}
          required
        />
        <input
          className="transition-soft rounded-xl border border-neutral-200 px-4 py-3 outline-none focus:border-neutral-400 focus:shadow-sm"
          type="password"
          placeholder="Пароль"
          autoComplete="current-password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          required
        />

        {error && <p className="animate-fade text-center text-sm text-red-500">{error}</p>}

        <button
          type="submit"
          disabled={busy}
          className="transition-soft rounded-xl bg-neutral-900 py-3 text-lg text-white hover:bg-neutral-700 active:scale-[0.98] active:opacity-80 disabled:opacity-40"
        >
          {busy ? "Входим…" : "Войти"}
        </button>
      </form>
    </main>
  );
}
