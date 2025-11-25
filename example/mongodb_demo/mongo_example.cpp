#include <iostream>
#include <string>
#include <cstdlib>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#include "pinpoint/tracer.h"

// Helper to trace MongoDB operations
template <typename Func>
void trace_mongo_op(pinpoint::SpanPtr span, const std::string& operation_name, const std::string& collection_name, const std::string& query_desc, Func func) {
    auto se = span->NewSpanEvent(operation_name);
    se->SetServiceType(pinpoint::SERVICE_TYPE_MONGODB_QUERY);
    se->SetEndPoint("localhost:27017"); // Ideally, get this from URI
    se->SetDestination("mongodb-demo");
    
    // Record collection and query details
    // Pinpoint MongoDB plugin usually records collection info in specific annotations
    // Since we don't have those exact constants exposed in the basic header, we'll use SQL/Generic fields
    // or custom annotations if available. For now, we put collection info in description or as SQL.
    std::string full_desc = "Collection: " + collection_name + ", Query: " + query_desc;
    se->SetSqlQuery(full_desc, ""); 

    try {
        func();
    } catch (const std::exception& e) {
        se->SetError(e.what());
        std::cerr << "Error in " << operation_name << ": " << e.what() << std::endl;
    }
    
    span->EndSpanEvent();
}

int main(int argc, char* argv[]) {
    std::string mongo_uri_str = "mongodb://localhost:27017";
    if (const char* env_uri = std::getenv("MONGO_URI")) {
        mongo_uri_str = env_uri;
    }

    // Pinpoint Agent Init
    pinpoint::SetConfigString(
        "AgentId=mongodb-demo-agent\n"
        "ApplicationName=MongoDBDemoApp\n"
        "Collector.Ip=127.0.0.1\n"
        "Collector.GrpcPort=9991\n"
        "Collector.StatPort=9992\n"
        "Collector.SpanPort=9993"
    );
    auto agent = pinpoint::CreateAgent();
    
    // Start a Trace
    auto span = agent->NewSpan("MongoDBDemo", "/mongo-example");
    span->SetServiceType(pinpoint::SERVICE_TYPE_CPP);
    span->SetRemoteAddress("127.0.0.1");
    span->SetEndPoint("mongodb-demo");

    try {
        // MongoDB C++ Driver Init
        mongocxx::instance instance{}; 
        mongocxx::uri uri(mongo_uri_str);
        mongocxx::client client(uri);
        
        auto db = client["testdb"];
        auto collection = db["testcollection"];

        // 1. Insert
        trace_mongo_op(span, "Insert", "testcollection", "Insert {name: 'MongoDB', type: 'database'}", [&]() {
            bsoncxx::builder::stream::document document{};
            document << "name" << "MongoDB"
                     << "type" << "database"
                     << "count" << 1
                     << "info" << bsoncxx::builder::stream::open_document
                        << "x" << 203
                        << "y" << 102
                     << bsoncxx::builder::stream::close_document;

            auto result = collection.insert_one(document.view());
            if (result) {
                std::cout << "Inserted document with id: " << result->inserted_id().get_oid().value.to_string() << std::endl;
            }
        });

        // 2. Find
        trace_mongo_op(span, "Find", "testcollection", "Find {name: 'MongoDB'}", [&]() {
            bsoncxx::builder::stream::document filter_builder{};
            filter_builder << "name" << "MongoDB";
            
            auto cursor = collection.find(filter_builder.view());
            for (auto&& doc : cursor) {
                std::cout << "Found document: " << bsoncxx::to_json(doc) << std::endl;
            }
        });

        // 3. Update
        trace_mongo_op(span, "Update", "testcollection", "Update {name: 'MongoDB'} set {i: 110}", [&]() {
            bsoncxx::builder::stream::document filter_builder{};
            filter_builder << "name" << "MongoDB";

            bsoncxx::builder::stream::document update_builder{};
            update_builder << "$set" << bsoncxx::builder::stream::open_document
                           << "i" << 110
                           << bsoncxx::builder::stream::close_document;

            auto result = collection.update_one(filter_builder.view(), update_builder.view());
            if (result) {
                 std::cout << "Matched " << result->matched_count() << " documents, modified " << result->modified_count() << std::endl;
            }
        });
        
        // 4. Delete
        trace_mongo_op(span, "Delete", "testcollection", "Delete {name: 'MongoDB'}", [&]() {
            bsoncxx::builder::stream::document filter_builder{};
            filter_builder << "name" << "MongoDB";

            auto result = collection.delete_one(filter_builder.view());
            if (result) {
                std::cout << "Deleted " << result->deleted_count() << " documents" << std::endl;
            }
        });

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        span->SetError(e.what());
    }

    span->EndSpan();
    agent->Shutdown();
    
    return 0;
}

