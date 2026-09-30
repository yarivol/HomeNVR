"use client";

// Проверка авторизации: возвращает пользователя или редиректит на /login.
// Если setup не завершён — редирект на /setup.
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";

export interface User {
  id: number;
  username: string;
  role: "USER" | "ADMIN";
}

export function useAuth(): { user: User | null; loading: boolean } {
  const [user, setUser] = useState<User | null>(null);
  const [loading, setLoading] = useState(true);
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
          router.replace("/login");
          return;
        }
        setUser(await res.json());
      } catch {
        router.replace("/login");
      } finally {
        setLoading(false);
      }
    })();
  }, [router]);

  return { user, loading };
}
