#pragma once
// PostgreSQL: подключение с retry, миграции, транзакции.
// Одна камера = низкая нагрузка, достаточно одного соединения под мьютексом.
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <pqxx/pqxx>

namespace db {

class Database {
public:
    explicit Database(std::string url);

    // Подключение с повторными попытками (postgres может стартовать дольше)
    void connect_with_retry(int attempts = 30, int delay_sec = 2);

    // Применяет *.sql из директории по порядку, каждый файл — один раз
    void run_migrations(const std::string& dir);

    // Выполнить функцию внутри транзакции (потокобезопасно).
    // Работает и с void-лямбдами, и с возвращающими значение.
    // Вложенный вызов tx() из лямбды tx() запрещён (был бы самодедлок) —
    // вместо зависания бросаем исключение.
    // При обрыве соединения (postgres перезапустился) переподключаемся
    // и повторяем транзакцию один раз — иначе до рестарта backend'а
    // все запросы к БД падали бы навсегда.
    template <typename F>
    auto tx(F&& fn) -> decltype(fn(std::declval<pqxx::work&>())) {
        if (in_tx_)
            throw std::logic_error("nested Database::tx is not allowed");
        std::lock_guard lock(mutex_);
        struct Guard {
            bool& flag;
            explicit Guard(bool& f) : flag(f) { flag = true; }
            ~Guard() { flag = false; }
        } guard(in_tx_);
        for (int attempt = 0;; ++attempt) {
            try {
                pqxx::work w{*conn_};
                if constexpr (std::is_void_v<decltype(fn(w))>) {
                    fn(w);
                    w.commit();
                    return;
                } else {
                    auto result = fn(w);
                    w.commit();
                    return result;
                }
            } catch (const pqxx::broken_connection&) {
                if (attempt >= 1) throw;
                // соединение умерло — разрыв до commit откатывает транзакцию
                // на сервере, повтор безопасен
                conn_ = std::make_unique<pqxx::connection>(url_);
            }
        }
    }

private:
    std::string url_;
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex mutex_;
    // Детект вложенных tx() в том же потоке (один экземпляр Database на процесс)
    inline static thread_local bool in_tx_ = false;
};

}  // namespace db
