#include <iostream>
#include <string>
#include <array>         // для буфера в exec
#include <memory>        // для unique_ptr в exec
#include <stdexcept>     // для runtime_error
#include <cstring>       // для memset и strncpy
#include <sys/socket.h>  // socket, bind, listen, accept, recv, send
#include <sys/un.h>      // sockaddr_un — адрес сокет-файла
#include <netinet/in.h>  // sockaddr_in — адрес с портом, htons
#include <unistd.h>      // close, unlink

using namespace std;

// функция из методички: запускает команду cmd и возвращает ее вывод строкой
std::string exec(string cmd) {
	std::array<char, 128> buffer;
	std::string result;
	std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
	if (!pipe) {
		throw std::runtime_error("popen() failed!");
	}
	while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
		result += buffer.data();
	}
	return result;
}

// оборачивает текст в одинарные кавычки, чтобы безопасно передать его в команду.
// внутри одинарных кавычек bash ничего не выполняет, а саму кавычку ' заменяем на '\''
string quote(string text) {
	string result = "'";
	for (char c : text) {
		if (c == '\'') result += "'\\''";
		else result += c;
	}
	result += "'";
	return result;
}

// отправляет строку клиенту.
// MSG_NOSIGNAL — если клиент уже отключился, не убивать программу сигналом SIGPIPE
void send_text(int client_fd, string text) {
	send(client_fd, text.c_str(), text.size(), MSG_NOSIGNAL);
}

int main(int argc, char* argv[]) {
	string socket_path = "daemon.socket"; // по умолчанию — сокет-файл в текущей папке
	int port = 0;                          // 0 — значит порт не указан

	// разбираем аргументы: ./main --socket путь   или   ./main --port номер
	for (int i = 1; i < argc; i++) {
		string arg = argv[i];
		if (arg == "--socket" && i + 1 < argc) {
			socket_path = argv[i + 1];
			i++; // значение уже забрали, пропускаем его
		}
		else if (arg == "--port" && i + 1 < argc) {
			port = stoi(argv[i + 1]); // stoi переводит строку в число
			i++;
		}
		else {
			cout << "использование: ./main [--socket путь] или ./main [--port номер]" << endl;
			return 1;
		}
	}

	int server_fd; // номер (дескриптор) нашего серверного сокета

	if (port > 0) {
		// ===== вариант 1: сетевой порт =====
		// AF_INET — сеть IPv4, SOCK_STREAM — протокол TCP
		server_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (server_fd < 0) {
			cout << "не удалось создать сокет" << endl;
			return 1;
		}

		// разрешаем сразу занять порт заново после перезапуска демона
		int opt = 1;
		setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

		// заполняем адрес: на каком IP и порту слушать
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));     // обнуляем структуру
		addr.sin_family = AF_INET;          // тип адреса — IPv4
		addr.sin_addr.s_addr = INADDR_ANY;  // слушать на всех IP этого компьютера
		addr.sin_port = htons(port);        // порт в сетевом порядке байт

		// привязываем сокет к адресу
		if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
			cout << "не удалось занять порт " << port << endl;
			return 1;
		}
		cout << "Слушаю запросы на порту " << port << endl;
	}
	else {
		// ===== вариант 2: сокет-файл =====
		// если файл остался от прошлого запуска — удаляем, иначе bind не сработает
		unlink(socket_path.c_str());

		// AF_UNIX — сокет внутри одного компьютера через файл
		server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (server_fd < 0) {
			cout << "не удалось создать сокет" << endl;
			return 1;
		}

		// заполняем адрес: путь к сокет-файлу
		sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

		// привязываем сокет к файлу (в этот момент файл и появляется)
		if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
			cout << "не удалось создать сокет-файл " << socket_path << endl;
			return 1;
		}
		cout << "Слушаю запросы на сокет-файле " << socket_path << endl;
	}

	// начинаем слушать подключения (5 — сколько клиентов может ждать в очереди)
	listen(server_fd, 5);

	// демон работает бесконечно
	while (true) {
		cout << "Жду приказов" << endl;

		// блокируемся, пока кто-нибудь не подключится
		int client_fd = accept(server_fd, NULL, NULL);
		if (client_fd < 0) {
			continue; // подключение не удалось — ждем следующее
		}
		cout << "Кто-то подключился" << endl;
		send_text(client_fd, "Привет! Напиши текст латиницей, и я нарисую его через figlet\n");

		// общаемся с подключившимся клиентом
		while (true) {
			char buffer[1024];

			// ждем, пока клиент что-нибудь пришлет
			int n = recv(client_fd, buffer, sizeof(buffer), 0);

			// 0 — клиент отключился (Ctrl+C в socat), меньше 0 — ошибка
			if (n <= 0) {
				cout << "Произошёл разрыв соединения" << endl;
				close(client_fd);
				break; // выходим к ожиданию нового клиента
			}

			// превращаем ровно n полученных байт в строку
			string input(buffer, n);

			// убираем перевод строки в конце (socat отправляет текст вместе с Enter)
			while (!input.empty() && (input.back() == '\n' || input.back() == '\r')) {
				input.pop_back();
			}
			if (input == "") {
				continue; // пустую строку не обрабатываем
			}

			cout << "получил: " << input << endl;

			// команда из прошлой лабы: проверка автоперезапуска через systemd
			if (input.find("Умри") != string::npos) {
				cout << "получил команду Умри, падаю" << endl;
				throw runtime_error("Падаю =ООО");
			}

			// рисуем текст через figlet и отправляем клиенту
			string art = exec("figlet " + quote(input));
			send_text(client_fd, art);

			cout << "отправил:" << endl << art << endl;
		}
	}

	close(server_fd);
	return 0;
}
