build:
	@g++ main.cpp -o main -Wno-ignored-attributes

socket: build
	@./main --socket daemon.socket

port: build
	@./main --port 8080

restart: build
	@sudo systemctl restart socket_daemon.service
	@echo "демон пересобран и перезапущен"
