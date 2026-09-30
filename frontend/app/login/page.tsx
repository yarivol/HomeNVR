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
        className="flex w-full max-w-sm flex-col gap-4 rounded-3xl bg-white p-8 shadow-sm"
      >
        <h1 className="text-center text-2xl font-semibold">HomeNVR</h1>

        <input
          className="rounded-xl border border-neutral-200 px-4 py-3 outline-none focus:border-neutral-400"
          placeholder="Имя пользователя"
          autoComplete="username"
          value={username}
          onChange={(e) => setUsername(e.target.value)}
          required
        />
        <input
          className="rounded-xl border border-neutral-200 px-4 py-3 outline-none focus:border-neutral-400"
          type="password"
          placeholder="Пароль"
          autoComplete="current-password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          required
        />

        {error && <p className="text-center text-sm text-red-500">{error}</p>}

        <button
          type="submit"
          disabled={busy}
          className="rounded-xl bg-neutral-900 py-3 text-lg text-white active:opacity-80 disabled:opacity-40"
        >
          Войти
        </button>
      </form>
    </main>
  );
}
