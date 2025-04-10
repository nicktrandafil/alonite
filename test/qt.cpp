#include <alonite/mpsc.h>
#include <alonite/qt.h>

#include <catch2/catch_all.hpp>

#include <QChronoTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

using namespace alonite;

TEST_CASE("", "[QExecutor]") {
    QExecutor executor;
}

class MyObject : public QObject {
    Q_OBJECT

public:
    void emit_my_signal(int x) {
        emit my_signal(x);
    }

    void emit_my_signal0() {
        emit my_signal0();
    }

    void emit_my_signal2(int x, std::string const& y) {
        emit my_signal2(x, y);
    }

signals:
    void my_signal(int);
    void my_signal0();
    void my_signal2(int, std::string const&);
};

TEST_CASE("wait a signal", "[connect][qt]") {
    using namespace std::chrono_literals;

    int c = 0;
    QCoreApplication app(c, nullptr);
    QEventLoop qexec;
    MyObject my_object;
    QTimer timer;
    QTimer::singleShot(10ms, &my_object, [&my_object] {
        my_object.emit_my_signal(10);
    });
    QTimer::singleShot(15ms, &qexec, [&qexec] {
        qexec.quit();
    });

    ThisThreadExecutor exec;
    std::thread t{[&my_object, &exec] {
        exec.block_on([](auto& my_object) -> Task<void> {
            auto [tx, rx] = mpsc::unbound_channel<int>();
            connect(my_object, &MyObject::my_signal, tx);
            auto res = co_await rx.recv();
            REQUIRE(res == 10);
            co_return;
        }(my_object));
    }};

    qexec.exec();
    t.join();
}

TEST_CASE("wait a void signal", "[connect][qt]") {
    using namespace std::chrono_literals;

    int c = 0;
    QCoreApplication app(c, nullptr);
    QEventLoop qexec;
    MyObject my_object;
    QTimer timer;
    QTimer::singleShot(10ms, &my_object, [&my_object] {
        my_object.emit_my_signal0();
    });
    QTimer::singleShot(15ms, &qexec, [&qexec] {
        qexec.quit();
    });

    ThisThreadExecutor exec;
    std::thread t{[&my_object, &exec] {
        exec.block_on([](auto& my_object) -> Task<void> {
            auto [tx, rx] = mpsc::unbound_channel<void>();
            connect(my_object, &MyObject::my_signal0, tx);
            auto res = co_await rx.recv();
            REQUIRE(res == true);
            co_return;
        }(my_object));
    }};

    qexec.exec();
    t.join();
}

TEST_CASE("wait a multi arguments signal", "[connect][qt]") {
    using namespace std::chrono_literals;

    int c = 0;
    QCoreApplication app(c, nullptr);
    QEventLoop qexec;
    MyObject my_object;
    QTimer timer;
    QTimer::singleShot(10ms, &my_object, [&my_object] {
        my_object.emit_my_signal2(1, "hello world");
    });
    QTimer::singleShot(15ms, &qexec, [&qexec] {
        qexec.quit();
    });

    ThisThreadExecutor exec;
    std::thread t{[&my_object, &exec] {
        exec.block_on([](auto& my_object) -> Task<void> {
            auto [tx, rx] = mpsc::unbound_channel<std::tuple<int, std::string>>();
            connect(my_object, &MyObject::my_signal2, tx);
            auto const res = co_await rx.recv();
            REQUIRE(res == std::tuple{1, std::string{"hello world"}});
            co_return;
        }(my_object));
    }};

    qexec.exec();
    t.join();
}

TEST_CASE("wait a signal twice", "[Signal][qt]") {
    using namespace std::chrono_literals;

    int c = 0;
    QCoreApplication app(c, nullptr);
    MyObject my_object;
    QTimer timer;
    QTimer::singleShot(10ms, &my_object, [&my_object] {
        my_object.emit_my_signal(10);
    });
    QTimer::singleShot(11ms, &my_object, [&my_object] {
        my_object.emit_my_signal(11);
    });

    ThisThreadExecutor exec;
    std::thread t{[&my_object, &exec, &app] {
        exec.block_on([](auto& my_object, auto& app) -> Task<void> {
            auto [tx, rx] = mpsc::unbound_channel<int>();
            connect(my_object, &MyObject::my_signal, std::move(tx));
            auto const r1 = co_await rx.recv();
            REQUIRE(r1 == 10);
            auto const r2 = co_await rx.recv();
            REQUIRE(r2 == 11);
            app.quit();
            co_return;
        }(my_object, app));
    }};

    app.exec();
    t.join();
}

TEST_CASE("How to get into coroutine context", "[QExecutor][qt]") {
    using namespace std::chrono_literals;
    int c = 0;
    QCoreApplication app(c, nullptr);
    QExecutor exec;
    spawn([](auto& app) -> Task<void> {
        co_await Yield{};
        app.quit();
        co_return;
    }(app));
    app.exec();
}

#include "qt.moc"
