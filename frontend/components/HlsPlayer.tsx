"use client";

// HLS-плеер: нативный HLS в Safari, hls.js в остальных браузерах.
import Hls from "hls.js";
import { useEffect, useRef } from "react";

export default function HlsPlayer({ src, live = false }: { src: string; live?: boolean }) {
  const videoRef = useRef<HTMLVideoElement>(null);

  useEffect(() => {
    const video = videoRef.current;
    if (!video) return;

    // Приоритет — hls.js (MSE): некоторые Chromium отвечают "maybe" на
    // canPlayType(mpegurl), но нативно HLS не играют. Нативный — только fallback.
    if (Hls.isSupported()) {
      const hls = new Hls(
        live
          ? { liveSyncDurationCount: 3, maxLiveSyncPlaybackRate: 1.5 }
          : {}
      );
      hls.loadSource(src);
      hls.attachMedia(video);
      if (live) hls.on(Hls.Events.MANIFEST_PARSED, () => video.play().catch(() => {}));
      return () => hls.destroy();
    }
    if (video.canPlayType("application/vnd.apple.mpegurl")) {
      video.src = src;
      video.play().catch(() => {});
    }
  }, [src, live]);

  return (
    <video
      ref={videoRef}
      controls
      muted={live}
      autoPlay={live}
      playsInline
      className="w-full rounded-2xl bg-black shadow-sm"
    />
  );
}
