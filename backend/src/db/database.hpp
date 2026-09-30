#pragma once
// PostgreSQL: подключение с retry, миграции, транзакции.
// Одна камера = низкая нагрузка, достаточно одного соединения под мьютексом.
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <pqxx/pqxx>

namespace db {

class Database {
public:
    explicit Database(std::string url);

    // Подключение с повторными попытками (postgres может стартовать дольше)
    void connect_with_retry(int attempts = 30, int delay_sec = 2);

    // Применяет *.sql из директории по порядку, каждый файл — один раз
    void run_migrations(const std::string& dir);

    // Выполнить функцию внутри транзакции (потокобезопасно)
    template <typename F>
    auto tx(F&& fn) -> decltype(fn(std::declval<pqxx::work&>())) {
        std::lock_guard lock(mutex_);
        pqxx::work w{*conn_};
        auto result = fn(w);
        w.commit();
        return result;
    }

private:
    std::string url_;
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex mutex_;
};

}  // namespace db
