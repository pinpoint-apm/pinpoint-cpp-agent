#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <librdkafka/rdkafkacpp.h>
#include "pinpoint/tracer.h"
#include "kafka_trace_context.h"

static volatile sig_atomic_t run = 1;

static void sigterm(int sig) {
  run = 0;
}

int main(int argc, char **argv) {
    std::string brokers = "localhost:9092";
    std::string group = "test-group";
    std::string topic_str = "test-topic";

    if (const char* env_brokers = std::getenv("KAFKA_BROKERS")) {
        brokers = env_brokers;
    }
    
    // Pinpoint Agent Init
    pinpoint::SetConfigString(
        "AgentId=kafka-consumer\n"
        "ApplicationName=KafkaConsumerApp\n"
        "Collector.Ip=127.0.0.1\n"
        "Collector.GrpcPort=9991\n"
        "Collector.StatPort=9992\n"
        "Collector.SpanPort=9993"
    );
    auto agent = pinpoint::CreateAgent();

    std::string errstr;
    RdKafka::Conf *conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
    conf->set("metadata.broker.list", brokers, errstr);
    conf->set("group.id", group, errstr);
    conf->set("auto.offset.reset", "earliest", errstr);

    RdKafka::KafkaConsumer *consumer = RdKafka::KafkaConsumer::create(conf, errstr);
    if (!consumer) {
        std::cerr << "Failed to create consumer: " << errstr << std::endl;
        delete conf;
        return 1;
    }
    delete conf;

    std::vector<std::string> topics;
    topics.push_back(topic_str);
    RdKafka::ErrorCode err = consumer->subscribe(topics);
    if (err) {
        std::cerr << "Failed to subscribe to " << topics[0] << ": " << RdKafka::err2str(err) << std::endl;
        delete consumer;
        return 1;
    }

    signal(SIGINT, sigterm);
    signal(SIGTERM, sigterm);

    std::cout << "Consuming messages from " << topic_str << "..." << std::endl;

    while (run) {
        RdKafka::Message *msg = consumer->consume(1000);
        if (msg->err() == RdKafka::ERR_NO_ERROR) {
            // Found message
            RdKafka::Headers* headers = msg->headers(); // can be null
            
            // Create Span with parent context
            std::shared_ptr<pinpoint::Span> span;
            if (headers) {
                KafkaTraceContextReader reader(headers);
                span = agent->NewSpan("KafkaConsume", "kafka_consumer", reader);
            } else {
                span = agent->NewSpan("KafkaConsume", "kafka_consumer");
            }
            
            span->SetServiceType(pinpoint::SERVICE_TYPE_KFAKA);
            span->SetEndPoint(brokers);
            span->SetRemoteAddress(brokers);
            
            // We can record the message payload or offset as annotation if needed
            if (msg->payload()) {
                std::string payload(static_cast<const char *>(msg->payload()), msg->len());
                std::cout << "Consumed message: " << payload << std::endl;
            }
            
            span->EndSpan();
        }
        delete msg;
    }
    
    consumer->close();
    delete consumer;
    
    return 0;
}

