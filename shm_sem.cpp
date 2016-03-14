#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <limits>
#include <cassert>

#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>	
#include <sys/stat.h>
#include <semaphore.h>
#include <errno.h>
#include <unistd.h>
#include <wait.h>
#include <time.h>

using namespace std;

/*

Две разделяемые памяти:

1) Буфер, куда все процессы добавляют необработанные директории.
Буфер для удобства имеет формат стека.
Добавляем в конец, вынимаем также с конца.

Структура:
- В начале хранится размер всего буфера (size_t)
- Далее
[путь1 - массив char][длина пути1 - size_t]
....
[путьN - массив char][длина путиN - size_t]

2) Память для хранения ответа
Структура:
- Минимальное время (time_t)
- Длина мин. пути (size_t)
- Мин. путь (массив char)

*/

const char* SHM_PATH_BUF_NAME = "/shm_path_buf"; // имя разделяемой памяти для хранения буфера путей необработанных директорий
const char* SHM_ANSWER_NAME = "/shm_answer"; // имя разделяемой памяти для хранения ответа

const int SHM_PATH_BUF_MAXLEN = 1000000; // макс размер буфера путей

const int MAX_PATH_LEN = 256; // максимальная длина пути для ответа
const int SHM_ANSWER_MAXLEN = sizeof(time_t) + sizeof(size_t) + MAX_PATH_LEN; // макс размер памяти для ответа, должно хватать на данные: время создания, длина пути, путь

const char* SEM_PATH_BUF_NAME = "/sem_pathbuf"; // имя cемафора для доступа к буферу путей необработанных директорий
const char* SEM_ANSWER_NAME = "/sem_answer"; // имя семафора для доступа к ответу

/*
Функция процесса обработки директории:
1) взять необработанную директорию из буфера путей
если таких не осталось, то выйти из функции
2) пробежаться по всем элементам директории:
- если файл, то найти наименьшее время => в конце обновить ответ
- если директория, то добавить в буфер необработанных директорий
3) вызвать себя рекурсивно
*/
void process_directory(int no) {
	// семафор: получить доступ к буферу путей
	sem_t* sem_path_buf = sem_open(SEM_PATH_BUF_NAME, 0);
	sem_wait(sem_path_buf);

	// получить доступ к буферу путей
	int shm_pathbuf_id = shm_open(SHM_PATH_BUF_NAME, O_RDWR, 0777);

	// получить указатель на память буфера путей
	void* path_buf = mmap(0, SHM_PATH_BUF_MAXLEN, PROT_WRITE | PROT_READ, MAP_SHARED, shm_pathbuf_id, 0);

	// достать из буфера фактический размер буфера
	size_t path_buf_size = *(size_t*)path_buf;
	
	assert(path_buf_size >= sizeof(size_t)); // в буфере должен храниться хотя бы размер

	bool path_found = false; // признак: удалось достать из буфера путь или нет
	string path; // в эту переменную достаём необработанный путь
	if (path_buf_size > sizeof(size_t)) { // если такой путь есть в буфере
		path_found = true;

		// достать из буфера длину пути
		size_t path_len = *(size_t*)((char*)path_buf + path_buf_size - sizeof(size_t));

		// достать из буфера путь
		path = string((char*)path_buf + path_buf_size - sizeof(size_t) - path_len, (char*)path_buf + path_buf_size - sizeof(size_t));

		// обновить размер буфера (последний путь и его длину убрали)
		*(size_t*)path_buf -= sizeof(size_t) + path_len;

		cout << "i'm agent " << no << " working with path " << path << endl;
	}

	time_t min_time = std::numeric_limits<time_t>::max(); // минимальное время, изначально установили в бесконечность
	string min_file_path; // соотв. путь к файлу

	// если из буфера был получен необработанный путь
	// пробегаем по этой директории
	if (path_found) {
		DIR* dfd = opendir(path.c_str());
		if (dfd != 0) {
			dirent* dp = readdir(dfd);
			while (dp != 0) {
				if (dp->d_type == DT_REG) {
					string fp = path + "/" + dp->d_name;
					struct stat fs;
					if (stat(fp.c_str(), &fs) == 0 && fs.st_mtime < min_time) {
						min_time = fs.st_mtime;
						min_file_path = fp;
					}
				}
				else if (dp->d_type == DT_DIR) {
					string dn = dp->d_name;
					if (dn != "." && dn != "..") {
						// формируем новый путь
						string new_path = path + "/" + dn;

						// проверяем что хватит места в буфере
						if (*(size_t*)path_buf + new_path.length() + sizeof(size_t) > SHM_PATH_BUF_MAXLEN) {
							perror("Not enough space in buffer for new path");
						}
						else {
							// записываем путь
							memcpy((char*)path_buf + *(size_t*)path_buf, new_path.data(), new_path.length());
							// записываем длину пути
							size_t new_path_len = new_path.length();
							memcpy((char*)path_buf + *(size_t*)path_buf + new_path.length(), &new_path_len, sizeof(new_path_len));
							// обновляем размер буфера
							*(size_t*)path_buf += new_path.length() + sizeof(size_t);
						}
					}
				}
				dp = readdir(dfd);
			}
			closedir(dfd);
		}
	}

	// закрываем доступ к памяти буфера путей
	munmap(path_buf, SHM_PATH_BUF_MAXLEN);
	close(shm_pathbuf_id);

	// семафор: отдать доступ к буферу путей
	sem_post(sem_path_buf);

	// если путь не был получен из буфера, то завершаем
	if (!path_found)
		return;

	//-------------------------------------------------------

	// семафор: получить доступ к ответу
	sem_t* sem_answer = sem_open(SEM_ANSWER_NAME, 0);
	sem_wait(sem_answer);

	// получить доступ к буферу ответа
	int shm_answer_id = shm_open(SHM_ANSWER_NAME, O_RDWR, 0777);

	// получить указатель на память ответа
	void* answer = mmap(0, SHM_ANSWER_MAXLEN, PROT_WRITE | PROT_READ, MAP_SHARED, shm_answer_id, 0);

	// если полученное время меньше, чем уже записанное, обновляем
	if (min_time < *(time_t*)answer) {
		// записываем время
		*(time_t*)answer = min_time;
		// записываем длину пути
		size_t min_file_path_len = min_file_path.length();
		memcpy((char*)answer + sizeof(time_t), &min_file_path_len, sizeof(min_file_path_len));
		// записываем путь
		memcpy((char*)answer + sizeof(time_t) + sizeof(size_t), min_file_path.data(), min_file_path.length());
	}
	
	// закрываем доступ к памяти буфера путей
	munmap(answer, SHM_ANSWER_MAXLEN);
	close(shm_answer_id);

	// семафор: отдать доступ к ответу
	sem_post(sem_answer);

	//-------------------------------------------------------
	
	// вызываем себя рекурсивно для продолжения обработки
	process_directory(no);
}

int main()
{
	// создаём разделяемую память для буфера путей
	int shm_pathbuf_id = shm_open(SHM_PATH_BUF_NAME, O_CREAT | O_RDWR, 0777);

	// создаём разделяемую память для ответа
	int shm_answer_id = shm_open(SHM_ANSWER_NAME, O_CREAT | O_RDWR, 0777);

	// задаём размер разделяемой памяти для буфера путей
	ftruncate(shm_pathbuf_id, SHM_PATH_BUF_MAXLEN);

	// задаём размер разделяемой памяти для ответа
	ftruncate(shm_answer_id, SHM_ANSWER_MAXLEN);

	// создаём семафор для буфера путей
	sem_t* sem_path_buf = sem_open(SEM_PATH_BUF_NAME, O_CREAT, 0777, 1);

	// cоздаём семафор для ответа
	sem_t* sem_answer = sem_open(SEM_ANSWER_NAME, O_CREAT, 0777, 1);

	// записываем начальные данные в буффер путей: путь "."
	void* path_buf = mmap(0, SHM_PATH_BUF_MAXLEN, PROT_WRITE | PROT_READ, MAP_SHARED, shm_pathbuf_id, 0);
	string start_path = ".";
	size_t start_path_len = start_path.length();
	*(size_t*)path_buf = sizeof(size_t) + start_path_len + sizeof(size_t);
	memcpy((char*)path_buf + sizeof(size_t), start_path.data(), start_path.length());
	memcpy((char*)path_buf + sizeof(size_t) + start_path_len, &start_path_len, sizeof(start_path_len));
	munmap(path_buf, SHM_PATH_BUF_MAXLEN);

	// записываем начальные данные в ответ: максимальное значение времени
	void* answer = mmap(0, SHM_ANSWER_MAXLEN, PROT_WRITE | PROT_READ, MAP_SHARED, shm_answer_id, 0);
	*(time_t*)answer = std::numeric_limits<time_t>::max();
	munmap(answer, SHM_ANSWER_MAXLEN);

	// запусаем производные процессы процессы (5 шт.)
	for (int i = 0; i < 5; i++) {
		if (fork() == 0) { // если ребёнок
			process_directory(i);
			exit(0);
		}
	}

	// в родительском процессе ожидаем окончания всех производных
	for (int i = 0; i < 5; i++)
		wait(0);

	// получаем ответ
	answer = mmap(0, SHM_ANSWER_MAXLEN, PROT_WRITE | PROT_READ, MAP_SHARED, shm_answer_id, 0);
	// получаем минимальное время
	time_t min_time = *(time_t*)answer;
	if (min_time == std::numeric_limits<time_t>::max()) { // если время не уменьшилось от максимального, значит файлов не было
		cout << "No files found" << endl;
	}
	else {
		// получаем длину пути
		size_t min_file_path_len = *(size_t*)((char*)answer + sizeof(time_t));
		// получаем путь
		string min_file_path((char*)answer + sizeof(time_t) + sizeof(size_t), (char*)answer + sizeof(time_t) + sizeof(size_t) + min_file_path_len);
		munmap(answer, SHM_ANSWER_MAXLEN);

		cout << "File with min time:" << endl;
		cout << min_file_path << endl;
		cout << ctime(&min_time) << endl;
	}
	
	// всё закрываем
	close(shm_pathbuf_id);
	close(shm_answer_id);
	shm_unlink(SHM_PATH_BUF_NAME);
	shm_unlink(SHM_ANSWER_NAME);

	sem_close(sem_path_buf);
	sem_close(sem_answer);
	sem_unlink(SEM_PATH_BUF_NAME);
	sem_unlink(SEM_ANSWER_NAME);
	
	return 0;
}