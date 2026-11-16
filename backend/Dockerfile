FROM python:3.12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends git curl build-essential pkg-config libssl-dev && rm -rf /var/lib/apt/lists/*
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
ENV PATH="/root/.cargo/bin:${PATH}"
WORKDIR /app
COPY . .
RUN pip wheel --wheel-dir=/wheels '.[playback]'
FROM python:3.12-slim
RUN useradd --uid 10001 --create-home app
COPY --from=build /wheels /wheels
RUN pip install --no-cache-dir --no-deps /wheels/* && rm -rf /wheels
USER app
EXPOSE 8000
CMD ["uvicorn", "switchmusic.app:app", "--host", "0.0.0.0", "--port", "8000", "--no-access-log"]
