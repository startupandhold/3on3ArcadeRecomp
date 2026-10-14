FROM python:3.12-slim

WORKDIR /app
COPY lobby.py tunnel.py nhl3on3_server.py ./

ENV PYTHONUNBUFFERED=1
EXPOSE 10901/tcp 30380/tcp 42127/tcp 3670/udp

CMD ["python", "nhl3on3_server.py"]
