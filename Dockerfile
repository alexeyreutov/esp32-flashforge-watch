FROM python:3.11-slim
RUN apt-get update && apt-get install -y --no-install-recommends git gcc g++ ca-certificates && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir platformio==6.1.19
WORKDIR /project
CMD ["pio", "run"]
