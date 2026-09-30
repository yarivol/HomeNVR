// Главная страница (ТЗ §11) — Phase 1: заглушка.
// Live preview, архив и события появятся в Phase 5.
export default function HomePage() {
  return (
    <main className="flex min-h-screen flex-col items-center justify-center gap-6 p-6">
      <h1 className="text-3xl font-semibold tracking-tight">HomeNVR</h1>
      <p className="text-neutral-500">Система запущена. Камера пока не настроена.</p>
      <div className="flex w-full max-w-xs flex-col gap-3">
        <button
          className="rounded-2xl bg-neutral-900 px-6 py-4 text-lg text-white active:opacity-80"
          disabled
        >
          Смотреть
        </button>
        <button
          className="rounded-2xl bg-white px-6 py-4 text-lg shadow-sm active:opacity-80"
          disabled
        >
          Архив
        </button>
      </div>
    </main>
  );
}
