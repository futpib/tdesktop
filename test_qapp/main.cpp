#include <QApplication>
#include <cstdio>
#include <time.h>

static long long usFromProcessStart() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	// Read process start time from /proc/self/stat
	// Simpler: just use a static baseline from first call
	static long long baseline = -1;
	long long now = (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
	if (baseline == -1) baseline = now;
	return now - baseline;
}

int main(int argc, char *argv[]) {
	long long t0 = usFromProcessStart();
	fprintf(stderr, "[%.3f] before QApplication\n", t0 / 1000.0);

	QApplication app(argc, argv);

	long long t1 = usFromProcessStart();
	fprintf(stderr, "[%.3f] after QApplication (delta: %.1f ms)\n", t1 / 1000.0, (t1 - t0) / 1000.0);
	fprintf(stderr, "Platform: %s\n", app.platformName().toUtf8().constData());
	return 0;
}
