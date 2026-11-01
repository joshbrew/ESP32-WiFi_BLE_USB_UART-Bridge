FROM node:22-alpine
WORKDIR /app/relay
COPY relay/package.json relay/package-lock.json ./
RUN npm ci --omit=dev --ignore-scripts && npm cache clean --force
COPY relay/*.mjs ./
COPY web/ /app/web/
USER node
EXPOSE 8080
CMD ["node", "server.mjs"]

