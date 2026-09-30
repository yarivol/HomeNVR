"use client";

// Проверка авторизации: возвращает пользователя или редиректит на /login.
// Если setup не завершён — редирект на /setup.
//
// Авторизованный пользователь кэшируется на уровне модуля: при SPA-переходах
// страница монтируется заново, и без кэша каждый раз 2 запроса (setup/status +
// auth/me) держали бы страницу пустой (белый экран). С кэшем рендер мгновенный,
// а сессия ревалидируется в фоне — 401/протухшая сессия всё равно разлогинит.
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";

export interface User {
  id: number;
  username: string;
  role: "USER" | "ADMIN";
}

let cachedUser: User | null = null;

// Сброс кэша при logout — иначе страницы продолжат рендериться для
// разлогиненного пользователя до фоновой ревалидации.
export function clearAuthCache() {
  cachedUser = null;
}

export function useAuth(): { user: User | null; loading: boolean } {
  const [user, setUser] = useState<User | null>(cachedUser);
  const [loading, setLoading] = useState(!cachedUser);
  const router = useRouter();

  useEffect(() => {
    (async () => {
      try {
        const setup = await fetch("/api/setup/status").then((r) => r.json());
        if (!setup.completed) {
          router.replace("/setup");
          return;
        }
        const res = await fetch("/api/auth/me", { credentials: "same-origin" });
        if (res.status === 401) {
          cachedUser = null;
          setUser(null);
          router.replace("/login");
          return;
        }
        cachedUser = await res.json();
        setUser(cachedUser);
      } catch {
        // сеть упала: разлогиниваем только если сессии нет в кэше,
        // иначе оставляем UI — фоновые API-вызовы сами уйдут на /login при 401
        if (!cachedUser) router.replace("/login");
      } finally {
        setLoading(false);
      }
    })();
  }, [router]);

  return { user, loading };
}
