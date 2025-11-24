#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <librdkafka/rdkafkacpp.h>
#include "pinpoint/tracer.h"
#include "kafka_trace_context.h"

static volatile sig_atomic_t run = 1;

static void sigterm(int sig) {
  run = 0;
}

class ExampleDeliveryReportCb : public RdKafka::DeliveryReportCb {
public:
  void dr_cb(RdKafka::Message &message) override {
    if (message.err())
      std::cerr << "% Message delivery failed: " << message.errstr() << std::endl;
    else
      std::cerr << "% Message delivered to topic " << message.topic_name() <<
        " [" << message.partition() << "] at offset " <<
        message.offset() << std::endl;
  }
};

int main(int argc, char **argv) {
    std::string brokers = "localhost:9092";
    std::string topic_str = "test-topic";

    if (const char* env_brokers = std::getenv("KAFKA_BROKERS")) {
        brokers = env_brokers;
    }
    
    // Pinpoint Agent Init
    // In a real scenario, these configs usually come from pinpoint-agent.conf
    pinpoint::SetConfigString(
        "AgentId=kafka-producer\n"
        "ApplicationName=KafkaProducerApp\n"
        "Collector.Ip=127.0.0.1\n"
        "Collector.GrpcPort=9991\n"
        "Collector.StatPort=9992\n"
        "Collector.SpanPort=9993"
    );
    auto agent = pinpoint::CreateAgent();

    // Kafka Config
    std::string errstr;
    RdKafka::Conf *conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
    conf->set("metadata.broker.list", brokers, errstr);
    
    ExampleDeliveryReportCb ex_dr_cb;
    conf->set("dr_cb", &ex_dr_cb, errstr);

    RdKafka::Producer *producer = RdKafka::Producer::create(conf, errstr);
    if (!producer) {
        std::cerr << "Failed to create producer: " << errstr << std::endl;
        delete conf;
        return 1;
    }
    delete conf;

    signal(SIGINT, sigterm);
    signal(SIGTERM, sigterm);

    std::cout << "Producing messages to " << topic_str << " (brokers: " << brokers << ")" << std::endl;

    int msg_cnt = 0;
    while (run) {
        // Create Span
        auto span = agent->NewSpan("KafkaProduce", "kafka_producer");
        span->SetServiceType(pinpoint::SERVICE_TYPE_KFAKA); 
        span->SetEndPoint(brokers);
        span->SetRemoteAddress(brokers);
        
        // Prepare headers
        RdKafka::Headers *headers = RdKafka::Headers::create();
        KafkaTraceContextWriter writer(headers);
        span->InjectContext(writer);
        
        std::string payload = "Message " + std::to_string(msg_cnt);
        
        // Produce
        RdKafka::ErrorCode resp = producer->produce(
            topic_str,
            RdKafka::Topic::PARTITION_UA,
            RdKafka::Producer::RK_MSG_COPY,
            const_cast<char *>(payload.c_str()), payload.size(),
            NULL, 0,
            0,
            headers, // headers are adopted by message on success
            NULL);

        if (resp != RdKafka::ERR_NO_ERROR) {
            std::cerr << "% Produce failed: " << RdKafka::err2str(resp) << std::endl;
            span->SetError(RdKafka::err2str(resp));
            delete headers; // Must delete headers if produce failed
        } else {
            producer->poll(0);
        }
        
        span->EndSpan();
        
        msg_cnt++;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cerr << "% Flushing final messages..." << std::endl;
    producer->flush(10 * 1000);

    if (producer->outq_len() > 0)
        std::cerr << "% " << producer->outq_len() << " message(s) were not delivered" << std::endl;

    delete producer;
    return 0;
}

