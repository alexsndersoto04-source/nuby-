FROM gcc:12-bookworm AS builder
WORKDIR /app
COPY . .
RUN cd nuby_engine && make clean && make all

FROM debian:bookworm-slim
WORKDIR /app
COPY --from=builder /app/nuby_engine/bin/nuby_engine /app/nuby_engine
EXPOSE 8080
ENV PORT=8080
CMD ["/app/nuby_engine", "--port", "8080"]
