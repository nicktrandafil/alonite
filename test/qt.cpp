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

signals:
    void my_signal(int);
};

TEST_CASE("wait a signal", "[Signal][qt]") {
    using namespace std::chrono_literals;

    int c = 0;
    QCoreApplication app(c, nullptr);
    QEventLoop qexec;
    MyObject my_object;
    QTimer timer;
    QTimer::singleShot(10ms, &my_object, [&my_object] { my_object.emit_my_signal(10); });
    QTimer::singleShot(15ms, &qexec, [&qexec] { qexec.quit(); });

    ThisThreadExecutor exec;
    std::thread t{[&my_object, &exec] {
        exec.block_on([](auto& my_object) -> Task<void> {
            auto res = co_await Signal{my_object, &MyObject::my_signal};
            REQUIRE(res == std::tuple(10));
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
    QTimer::singleShot(100ms, &my_object, [&my_object] {
        my_object.emit_my_signal(11);
    });

    ThisThreadExecutor exec;
    std::thread t{[&my_object, &exec, &app] {
        exec.block_on([](auto& my_object, auto& app) -> Task<void> {
            Signal signal{my_object, &MyObject::my_signal};
            auto const r1 = co_await signal;
            REQUIRE(r1 == std::tuple(10));
            auto const r2 = co_await signal;
            REQUIRE(r2 == std::tuple(11));
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
        app.quit();
        co_return;
    }(app));
    app.exec();
}

#include "qt.moc"
