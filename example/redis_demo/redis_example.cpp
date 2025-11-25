#include <iostream>
#include <cstdlib>
#include <cstring>
#include <hiredis.h>

#include "pinpoint/tracer.h"

void execute_command(redisContext *c, pinpoint::SpanPtr span, const char *format, ...) {
    va_list ap;
    void *reply = NULL;
    
    char command_buf[1024];
    va_start(ap, format);
    vsnprintf(command_buf, sizeof(command_buf), format, ap);
    va_end(ap);

    // Start Span Event for Redis Command
    auto span_event = span->NewSpanEvent("redis_command");
    span_event->SetServiceType(pinpoint::SERVICE_TYPE_REDIS);
    span_event->SetEndPoint("localhost:6379");
    span_event->SetDestination("redis-server");
    
    // Record the actual command
    auto annotations = span_event->GetAnnotations();
    // ANNOTATION_ARGS0 (41) is often used for args, but we can stick to conventions or just use a generic string
    // Pinpoint doesn't have a strict Redis command annotation constant exposed in the header provided, 
    // but usually SQL or generic arguments are fine. We'll use a custom key or just log it.
    // Let's assuming we might want to record the command string.
    // span_event->SetSqlQuery(command_buf); // Redis is not SQL, but this field is often used for query-like things.
    // Alternatively, just use description or error if failed.
    
    // Re-start va_list for redisCommand
    va_start(ap, format);
    reply = redisvCommand(c, format, ap);
    va_end(ap);

    redisReply *r = (redisReply*)reply;
    if (r == NULL) {
        std::cerr << "Error: " << c->errstr << std::endl;
        span_event->SetError(c->errstr);
    } else {
        if (r->type == REDIS_REPLY_ERROR) {
            std::cerr << "Redis Error: " << r->str << std::endl;
            span_event->SetError(r->str);
        } else {
             std::cout << "Command '" << command_buf << "' executed. ";
             if (r->type == REDIS_REPLY_STRING) {
                 std::cout << "Result: " << r->str << std::endl;
             } else if (r->type == REDIS_REPLY_INTEGER) {
                 std::cout << "Result: " << r->integer << std::endl;
             } else if (r->type == REDIS_REPLY_STATUS) {
                 std::cout << "Status: " << r->str << std::endl;
             } else {
                 std::cout << "Type: " << r->type << std::endl;
             }
        }
        freeReplyObject(reply);
    }
    
    span->EndSpanEvent();
}

int main(int argc, char **argv) {
    const char *hostname = "localhost";
    int port = 6379;
    
    if (const char* env_host = std::getenv("REDIS_HOST")) {
        hostname = env_host;
    }
    if (const char* env_port = std::getenv("REDIS_PORT")) {
        port = std::atoi(env_port);
    }

    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    redisContext *c = redisConnectWithTimeout(hostname, port, timeout);
    if (c == NULL || c->err) {
        if (c) {
            std::cerr << "Connection error: " << c->errstr << std::endl;
            redisFree(c);
        } else {
            std::cerr << "Connection error: can't allocate redis context" << std::endl;
        }
        exit(1);
    }

    // Initialize Pinpoint Agent
    pinpoint::SetConfigString(
        "AgentId=redis-demo-agent\n"
        "ApplicationName=RedisDemoApp\n"
        "Collector.Ip=127.0.0.1\n"
        "Collector.GrpcPort=9991\n"
        "Collector.StatPort=9992\n"
        "Collector.SpanPort=9993"
    );
    auto agent = pinpoint::CreateAgent();

    std::cout << "Connected to Redis at " << hostname << ":" << port << std::endl;

    // Start a new trace
    auto span = agent->NewSpan("RedisDemo", "/redis-example");
    span->SetServiceType(pinpoint::SERVICE_TYPE_CPP);
    span->SetRemoteAddress("127.0.0.1");
    span->SetEndPoint("redis-demo");

    // PING
    execute_command(c, span, "PING");

    // SET
    execute_command(c, span, "SET %s %s", "foo", "hello world");

    // GET
    execute_command(c, span, "GET foo");

    // INCR
    execute_command(c, span, "INCR counter");
    execute_command(c, span, "INCR counter");

    // Clean up list
    execute_command(c, span, "DEL mylist");

    // LPUSH loop
    for (int j = 0; j < 5; j++) {
        char buf[64];
        snprintf(buf, 64, "%d", j);
        execute_command(c, span, "LPUSH mylist element-%s", buf);
    }

    // LRANGE
    execute_command(c, span, "LRANGE mylist 0 -1");

    span->EndSpan();

    redisFree(c);
    agent->Shutdown();
    
    std::cout << "Done." << std::endl;

    return 0;
}

