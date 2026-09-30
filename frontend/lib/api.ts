// Хелпер для API-запросов. 401 → редирект на логин.
export async function api(path: string, options?: RequestInit): Promise<Response> {
  const res = await fetch(path, {
    credentials: "same-origin",
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  if (
    res.status === 401 &&
    typeof window !== "undefined" &&
    !window.location.pathname.startsWith("/login")
  ) {
    window.location.href = "/login";
  }
  return res;
}

export async function apiJson<T = unknown>(path: string, options?: RequestInit): Promise<T> {
  const res = await api(path, options);
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error((body as { error?: string }).error ?? `Ошибка ${res.status}`);
  }
  return res.json() as Promise<T>;
}
