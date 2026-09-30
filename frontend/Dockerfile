# --- Сборка ---
FROM node:22-alpine AS build

WORKDIR /app
COPY package.json package-lock.json ./
# npm ci — воспроизводимая установка строго по lock-файлу
RUN npm ci

COPY . .
RUN npm run build

# --- Запуск (standalone-режим Next.js) ---
FROM node:22-alpine

WORKDIR /app
ENV NODE_ENV=production
# Next standalone слушает на $HOSTNAME; в Docker это id контейнера —
# без 0.0.0.0 сервер недоступен на localhost (healthcheck) изнутри контейнера
ENV HOSTNAME=0.0.0.0

COPY --from=build /app/.next/standalone ./
COPY --from=build /app/.next/static ./.next/static
COPY --from=build /app/public ./public

EXPOSE 3000
CMD ["node", "server.js"]
