# Dockerfile for ns-3 with UCB Home internet traces and tools
# Command to run
# docker build . && docker run -it -v ./scratch:/ns-allinone-3.44/ns-3.44/scratch -v ./output:/output $(docker build -q .)
# This runs the files in scratch directory and outputs the logs to the logs directory

FROM ubuntu:latest

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    gcc \
    g++ \
    python3 \
    python3-dev \
    python3-pip \
    cmake \
    wget \
    curl \
    git \
    libgsl-dev \
    libboost-all-dev \
    libgtk-3-dev \
    libxml2-dev \
    libsqlite3-dev \
    python3-gi-cairo \
    && apt-get clean

# Create a working directory
WORKDIR /
# Clone ns-3
RUN wget https://www.nsnam.org/releases/ns-allinone-3.44.tar.bz2
RUN tar -xjf ns-allinone-3.44.tar.bz2
RUN rm ns-allinone-3.44.tar.bz2
WORKDIR /ns-allinone-3.44/ns-3.44
RUN ./ns3 configure --enable-examples --enable-tests
RUN ./ns3 build
RUN ./ns3 run first

WORKDIR /
# Download tools
COPY tools /tools
WORKDIR /tools
RUN make clean
RUN make

# Download traces
WORKDIR /traces
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-848278026-848292426.tr.gz
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-846890339-847313219.tr.gz
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-847313219-847601221.tr.gz
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-847601221-848004424.tr.gz
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-848004424-848409417.tr.gz
RUN gunzip UCB-home-IP-848278026-848292426.tr.gz
RUN gunzip UCB-home-IP-846890339-847313219.tr.gz
RUN gunzip UCB-home-IP-847313219-847601221.tr.gz
RUN gunzip UCB-home-IP-847601221-848004424.tr.gz
RUN gunzip UCB-home-IP-848004424-848409417.tr.gz
COPY ucb_trace_parser.py /traces
RUN python3 ucb_trace_parser.py UCB-home-IP-848278026-848292426.tr small_traces.txt
RUN python3 ucb_trace_parser.py UCB-home-IP-846890339-847313219.tr trace1.txt
RUN python3 ucb_trace_parser.py UCB-home-IP-847313219-847601221.tr trace2.txt
RUN python3 ucb_trace_parser.py UCB-home-IP-847601221-848004424.tr trace3.txt
RUN python3 ucb_trace_parser.py UCB-home-IP-848004424-848409417.tr trace4.txt
# Restore originals. Read using gunzip -c filename | /tools/showtrace
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-848278026-848292426.tr.gz
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-846890339-847313219.tr.gz
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-847313219-847601221.tr.gz
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-847601221-848004424.tr.gz
RUN wget ftp://ita.ee.lbl.gov/traces/UCB-home-IP-848004424-848409417.tr.gz


# Set the entry point
WORKDIR /ns-allinone-3.44/ns-3.44
ENTRYPOINT ["./ns3", "run", "scratch/http-trace-simulation --traceFile=/traces/small_traces.txt --mode=serial --bandwidth=1.5Mbps --delay=50ms —-time=10 --maxPages=0"]
# ENTRYPOINT ["./ns3", "run", "scratch/http-parallel-simulation --traceFile=/traces/small_traces.txt --mode=serial --bandwidth=1.5Mbps --delay=50ms —-time=10 --maxPages=0"]
# ENTRYPOINT ["./ns3", "run", "scratch/http-persistent-simulation --traceFile=/traces/small_traces.txt --mode=serial --bandwidth=1.5Mbps --delay=50ms —-time=10 --maxPages=0"]
# ENTRYPOINT ["./ns3", "run", "scratch/http-pipelined-simulation --traceFile=/traces/small_traces.txt --mode=serial --bandwidth=1.5Mbps --delay=50ms —-time=10 --maxPages=0"]
# ENTRYPOINT ["./ns3", "run", "scratch/http-sst-simulation --traceFile=/traces/small_traces.txt --mode=serial --bandwidth=1.5Mbps --delay=50ms —-time=10 --maxPages=0"]
# ENTRYPOINT [ "bash" ]

