#include "Prometheus.h"

// at least to start with, we're modelling this very closely after vkguide

int main () {
	std::cout << "Welcome To Prometheus." << std::endl;

	PrometheusInstance Prometheus;

	Prometheus.Init();
	Prometheus.MainLoop();
	Prometheus.ShutDown();

	return 0;
}
