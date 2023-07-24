FROM debian:bookworm-slim

RUN export DEBIAN_FRONTEND=noninteractive \
   && echo "deb http://deb.debian.org/debian/ sid main" > /etc/apt/sources.list.d/sid.list \
   && apt-get -y update \
   && apt-get -y install --no-install-recommends wget ca-certificates gnupg eatmydata \
   && eatmydata apt-get -y update \
   && eatmydata apt-get -y dist-upgrade \
   && cd /home/user/app \
   && eatmydata apt-get --no-install-recommends -y build-dep . \
   && eatmydata apt-get clean
