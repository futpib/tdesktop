// tdesktopctl — CLI tool for controlling a running tdesktop instance.
//
// Connects to the tdesktop control socket (tdesktop.sock) and
// exchanges line-delimited JSON messages.
//
// Protocol:
//   {"type":"tdlib", "payload":{"@type":"getMe", "@extra":"1"}}  — TDLib request
//   {"type":"tdesktop", "command":"ping"}                         — control command
//
// Usage:
//   tdesktopctl                              — interactive mode
//   tdesktopctl --exec '{"type":"tdlib","payload":{"@type":"getMe"}}'  — one-shot
//   tdesktopctl --socket /path/to/tdesktop.sock            — custom path
//   echo '...' | tdesktopctl                 — pipe mode

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kBufSize = 65536;

std::string defaultSocketPath() {
	// XDG_RUNTIME_DIR is the primary location (matches tdesktop server).
	const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR");
	if (runtimeDir && runtimeDir[0]) {
		return std::string(runtimeDir) + "/tdesktop.sock";
	}
	// Fallback: /tmp/tdesktop-<uid>/ per XDG Base Directory spec
	// (must match the server's fallback path).
	fprintf(stderr,
		"Warning: XDG_RUNTIME_DIR is not set, "
		"falling back to /tmp/tdesktop-%u/\n",
		static_cast<unsigned>(getuid()));
	return "/tmp/tdesktop-"
		+ std::to_string(static_cast<unsigned>(getuid()))
		+ "/tdesktop.sock";
}

int connectSocket(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	struct sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (std::strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "Socket path too long: %s\n", path);
		close(fd);
		return -1;
	}
	std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
		fprintf(stderr, "connect(%s): %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

// Read from fd into buf, process complete lines, write them to stdout.
// Returns false on EOF or error.
bool drainSocket(int fd, std::string &buf) {
	char tmp[kBufSize];
	ssize_t n = read(fd, tmp, sizeof(tmp));
	if (n <= 0) {
		return false;
	}
	buf.append(tmp, static_cast<size_t>(n));

	// Print complete lines.
	size_t pos;
	while ((pos = buf.find('\n')) != std::string::npos) {
		fwrite(buf.data(), 1, pos + 1, stdout);
		fflush(stdout);
		buf.erase(0, pos + 1);
	}
	return true;
}

// Send a line to the socket (appends newline if missing).
bool sendLine(int fd, const char *line) {
	size_t len = std::strlen(line);
	if (len == 0) {
		return true;
	}
	ssize_t w = write(fd, line, len);
	if (w < 0) {
		return false;
	}
	if (len == 0 || line[len - 1] != '\n') {
		write(fd, "\n", 1);
	}
	return true;
}

void printUsage(const char *argv0) {
	fprintf(stderr,
		"Usage: %s [OPTIONS]\n"
		"\n"
		"Options:\n"
		"  --socket PATH    Path to tdesktop.sock\n"
		"  --exec JSON      Send a single request and print the response\n"
		"  --help           Show this help\n"
		"\n"
		"Without --exec, reads JSON lines from stdin and writes responses to stdout.\n",
		argv0);
}

} // namespace

int main(int argc, char *argv[]) {
	const char *socketPath = nullptr;
	const char *execJson = nullptr;

	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socketPath = argv[++i];
		} else if (std::strcmp(argv[i], "--exec") == 0 && i + 1 < argc) {
			execJson = argv[++i];
		} else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
			printUsage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			printUsage(argv[0]);
			return 1;
		}
	}

	std::string pathStr;
	if (!socketPath) {
		pathStr = defaultSocketPath();
		socketPath = pathStr.c_str();
	}

	int fd = connectSocket(socketPath);
	if (fd < 0) {
		return 1;
	}

	std::string socketBuf;

	if (execJson) {
		// One-shot mode: send request, read one response, exit.
		if (!sendLine(fd, execJson)) {
			perror("write");
			close(fd);
			return 1;
		}

		// Read until we get a complete line.
		while (true) {
			char tmp[kBufSize];
			ssize_t n = read(fd, tmp, sizeof(tmp));
			if (n <= 0) {
				break;
			}
			socketBuf.append(tmp, static_cast<size_t>(n));
			size_t pos = socketBuf.find('\n');
			if (pos != std::string::npos) {
				fwrite(socketBuf.data(), 1, pos + 1, stdout);
				break;
			}
		}
		close(fd);
		return 0;
	}

	// Interactive / pipe mode: bridge stdin ↔ socket.
	struct pollfd fds[2];
	fds[0].fd = STDIN_FILENO;
	fds[0].events = POLLIN;
	fds[1].fd = fd;
	fds[1].events = POLLIN;

	std::string stdinBuf;

	while (true) {
		int ret = poll(fds, 2, -1);
		if (ret < 0) {
			if (errno == EINTR) continue;
			perror("poll");
			break;
		}

		// Data from socket → stdout.
		if (fds[1].revents & POLLIN) {
			if (!drainSocket(fd, socketBuf)) {
				break;
			}
		}
		if (fds[1].revents & (POLLHUP | POLLERR)) {
			break;
		}

		// Data from stdin → socket.
		if (fds[0].revents & POLLIN) {
			char tmp[kBufSize];
			ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
			if (n <= 0) {
				// stdin closed; keep reading responses then exit.
				fds[0].fd = -1;
				// Give the server a moment to respond.
				int timeout_ms = 500;
				while (poll(&fds[1], 1, timeout_ms) > 0) {
					if (!drainSocket(fd, socketBuf)) break;
					timeout_ms = 200;
				}
				break;
			}
			stdinBuf.append(tmp, static_cast<size_t>(n));

			// Send complete lines to socket.
			size_t pos;
			while ((pos = stdinBuf.find('\n')) != std::string::npos) {
				std::string line = stdinBuf.substr(0, pos);
				stdinBuf.erase(0, pos + 1);
				if (!line.empty()) {
					sendLine(fd, line.c_str());
				}
			}
		}
		if (fds[0].revents & POLLHUP) {
			// stdin closed.
			fds[0].fd = -1;
			int timeout_ms = 500;
			while (poll(&fds[1], 1, timeout_ms) > 0) {
				if (!drainSocket(fd, socketBuf)) break;
				timeout_ms = 200;
			}
			break;
		}
	}

	close(fd);
	return 0;
}
