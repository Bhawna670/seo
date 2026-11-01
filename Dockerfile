# Stage 1: compile the C++ program
FROM alpine:3.20 AS build
RUN apk add --no-cache g++
WORKDIR /src
COPY src ./src
RUN g++ -std=c++17 -O2 -static src/main.cpp src/Trie.cpp -o search_engine

# Stage 2: serve it in a browser terminal (ttyd). Visitors get the program itself, not a shell.
FROM alpine:3.20
RUN apk add --no-cache ttyd
WORKDIR /app
COPY --from=build /src/search_engine .
COPY Data ./Data
COPY run.sh .
ENV PORT=10000
EXPOSE 10000
CMD ["sh", "-c", "ttyd -W -m 10 -p ${PORT} sh run.sh"]
