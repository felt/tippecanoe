FROM ubuntu:22.04 AS tippecanoe-builder

RUN apt-get update \
  && apt-get -y install build-essential libsqlite3-dev zlib1g-dev

COPY . /tmp/tippecanoe-src
WORKDIR /tmp/tippecanoe-src

RUN make 

CMD make test

# Using multistage build reduces the docker image size by alot by only copying the needed binaries
FROM ubuntu:22.04
RUN apt-get update \
  && apt-get -y install libsqlite3-dev zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*
COPY --from=tippecanoe-builder /tmp/tippecanoe-src/tippecanoe* /usr/local/bin/
COPY --from=tippecanoe-builder /tmp/tippecanoe-src/tile-join /usr/local/bin/
WORKDIR /app 
