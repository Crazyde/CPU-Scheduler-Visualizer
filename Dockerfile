FROM python:3.12-slim

# Install build tools for compiling the C++ scheduler
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy project files
COPY . /app

# Build the C++ simulator to ./result
RUN g++ -O2 -std=c++17 -o result main.cpp

# Python dependencies
ENV PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1
RUN pip install --upgrade pip \
    && pip install -r requirements-render.txt

# Render provides PORT. Default to 5001 for local docker runs.
ENV PORT=5001

# Expose the service port (optional; informative)
EXPOSE 5001

# Start the Flask app via gunicorn.
# Use a shell so $PORT is expanded at runtime on Render (exec-form doesn't expand env vars).
CMD ["sh", "-c", "exec gunicorn -w 2 -k gthread --threads 4 -b 0.0.0.0:${PORT:-5001} server:app"]
