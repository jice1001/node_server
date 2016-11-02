/*
 * Mongo_Operator.cpp
 *
 *  Created on: Dec 29, 2015
 *      Author: zhangyalei
 */

#ifdef MONGO_DB_IMPLEMENT

#include "Base_Function.h"
#include "Base_V8.h"
#include "Data_Manager.h"
#include "Struct_Manager.h"
#include "Mongo_Operator.h"
#include "Base_Struct.h"

Mongo_Operator::Mongo_Operator(void) { }

Mongo_Operator::~Mongo_Operator(void) { }

DBClientConnection &Mongo_Operator::get_connection(int db_id) {
	Connection_Map::iterator iter = connection_map_.find(db_id);
	if(iter == connection_map_.end()){
		LOG_FATAL("DataBase %d dosen't connect", db_id);
	}
	return *(iter->second);
}

bool Mongo_Operator::connect_to_db(int db_id, std::string &ip, int port, std::string &user, std::string &password, std::string &pool_name) {
	DBClientConnection *conn = connection_pool_.pop();
	std::string err_msg;
	std::stringstream host_stream;
	host_stream << ip;
	host_stream << ":" << port;
	bool ret = conn->connect(host_stream.str().c_str(), err_msg);
	if(!ret)
		return false;
	connection_map_[db_id] = conn;
	return true;
}

int Mongo_Operator::init_db(int db_id, DB_Struct *db_struct) {
	//创建索引
	get_connection(db_id).createIndex(db_struct->table_name(), BSON(db_struct->index_name() << 1));
	db_struct->set_db_init();
	return 0;
}

int64_t Mongo_Operator::generate_table_index(int db_id, DB_Struct *db_struct, std::string &type) {
	if(!db_struct->db_init()) {
		init_db(db_id, db_struct);
	}

	int idx = 1;
	BSONObj result;
	char str_json[128] = {0};
	sprintf(str_json, "{findandmodify:'%s', query:{type:'%s'}, update:{$inc:{value:1}}, upsert:true}",
			db_struct->table_name().c_str(), type.c_str());
	if (get_connection(db_id).runCommand(db_struct->db_name(), fromjson(str_json), result) == false) {
		LOG_ERROR("findandmodify type='%s' failed", type.c_str());
		return -1;
	}

	idx = result.getFieldDotted("value.value").numberLong() + 1;
	int64_t id = make_id(STRUCT_MANAGER->agent_num(), STRUCT_MANAGER->server_num(), idx);
	return id;
}

int64_t Mongo_Operator::select_table_index(int db_id, DB_Struct *db_struct,  std::string &query_name, std::string &query_value) {
	if(!db_struct->db_init()) {
		init_db(db_id, db_struct);
	}
	std::string table = db_struct->table_name();
	BSONObj result = get_connection(db_id).findOne(table, MONGO_QUERY(query_name << query_value));
	if (!result.isEmpty()) {
		return result[db_struct->index_name()].numberLong();
	} else {
		return -1;
	}
}

/////////////////////////////////数据转成object///////////////////////////
v8::Local<v8::Object> Mongo_Operator::load_data(int db_id, DB_Struct *db_struct, Isolate* isolate, int64_t key_index) {
	if(!db_struct->db_init()) {
		init_db(db_id, db_struct);
	}

	EscapableHandleScope handle_scope(isolate);
	Local<Context> context(isolate->GetCurrentContext());
	if(key_index == 0) {
		//加载整张表数据
		std::vector<BSONObj> record;
		int len = get_connection(db_id).count(db_struct->table_name());
		if(len > 0) {
			get_connection(db_id).findN(record, db_struct->table_name(), Query(), len);
		}
		v8::Local<v8::Array> array = Array::New(isolate, len);
		for (int i = 0; i < len; ++i) {
			v8::Local<v8::Object> data_obj = load_data_single(db_struct, isolate, record[i]);
			if (!data_obj.IsEmpty()) {
				array->Set(context, i, data_obj).FromJust();
			}
		}
		return handle_scope.Escape(array);
	} else {
		//加载单条数据
		BSONObj bsonobj = get_connection(db_id).findOne(db_struct->table_name(), MONGO_QUERY(db_struct->index_name() << (long long int)key_index));
		v8::Local<v8::Object> data_obj = load_data_single(db_struct, isolate, bsonobj);
		return handle_scope.Escape(data_obj);
	}
}

void Mongo_Operator::save_data(int db_id, DB_Struct *db_struct, Isolate* isolate, v8::Local<v8::Object> object) {
	if(!db_struct->db_init()) {
		init_db(db_id, db_struct);
	}

	Local<Context> context(isolate->GetCurrentContext());
	BSONObjBuilder set_builder;
	v8::Local<v8::Value> key_value = object->Get(context, String::NewFromUtf8(isolate, db_struct->index_name().c_str(), NewStringType::kNormal).ToLocalChecked()).ToLocalChecked();
	int64_t key_index = key_value->NumberValue(context).FromJust();
	LOG_INFO("table %s save key_index:%ld", db_struct->table_name().c_str(), key_index);
	if (key_index <= 0) {
		return;
	}

	for(std::vector<Field_Info>::const_iterator iter = db_struct->field_vec().begin();
			iter != db_struct->field_vec().end(); iter++) {
		v8::Local<v8::Value> value = object->Get(context, String::NewFromUtf8(isolate, iter->field_name.c_str(), NewStringType::kNormal).ToLocalChecked()).ToLocalChecked();
		if(iter->field_label == "arg") {
			save_data_arg(db_struct, isolate, *iter, set_builder, value);
		}
		else if(iter->field_label == "vector") {
			save_data_vector(db_struct, isolate, *iter, set_builder, value);
		}
		else if(iter->field_label == "map") {
			save_data_map(db_struct, isolate, *iter, set_builder, value);
		}
		else if(iter->field_label == "struct") {
			save_data_struct(db_struct, isolate, *iter, set_builder, value);
		}
	}
	get_connection(db_id).update(db_struct->table_name(), MONGO_QUERY(db_struct->index_name() << (long long int)key_index),
			BSON("$set" << set_builder.obj() ), true);
}

/////////////////////////////////数据转成buffer///////////////////////////
int Mongo_Operator::load_data(int db_id, DB_Struct *db_struct, int64_t key_index, std::vector<Byte_Buffer *> &buffer_vec) {
	if(!db_struct->db_init()) {
		init_db(db_id, db_struct);
	}
	int len = 0;
	if(key_index == 0) {
		//加载整张表数据
		std::vector<BSONObj> record;
		len = get_connection(db_id).count(db_struct->table_name());
		if(len > 0) {
			get_connection(db_id).findN(record, db_struct->table_name(), Query(), len);
		}
		for (int i = 0; i < len; ++i) {
			Byte_Buffer *buffer = DATA_MANAGER->pop_buffer();
			load_data_single(db_struct, record[i], *buffer);
			buffer_vec.push_back(buffer);
		}
	} else {
		//加载单条数据
		BSONObj bsonobj = get_connection(db_id).findOne(db_struct->table_name(), MONGO_QUERY(db_struct->index_name() << (long long int)key_index));
		Byte_Buffer *buffer = DATA_MANAGER->pop_buffer();
		load_data_single(db_struct, bsonobj, *buffer);
		buffer_vec.push_back(buffer);
		len = 1;
	}
	return len;
}

void Mongo_Operator::save_data(int db_id, DB_Struct *db_struct, Byte_Buffer *buffer) {
	if(!db_struct->db_init()) {
		init_db(db_id, db_struct);
	}
	BSONObjBuilder set_builder;
	int64_t key_index = 0;
	buffer->peek_int64(key_index);
	LOG_INFO("table %s save key_index:%ld", db_struct->table_name().c_str(), key_index);
	if (key_index <= 0) {
		return;
	}

	for(std::vector<Field_Info>::const_iterator iter = db_struct->field_vec().begin();
			iter != db_struct->field_vec().end(); iter++) {
		if(iter->field_label == "arg") {
			save_data_arg(db_struct, *iter, set_builder, *buffer);
		}
		else if(iter->field_label == "vector" || iter->field_label == "map") {
			save_data_vector(db_struct, *iter, set_builder, *buffer);
		}
		else if(iter->field_label == "struct") {
			save_data_struct(db_struct, *iter, set_builder, *buffer);
		}
	}
	get_connection(db_id).update(db_struct->table_name(), MONGO_QUERY(db_struct->index_name() << (long long int)key_index),
			BSON("$set" << set_builder.obj() ), true);
}

void Mongo_Operator::delete_data(int db_id, DB_Struct *db_struct, Isolate* isolate, v8::Local<v8::Object> object) {
	if (!object->IsArray()) {
		LOG_ERROR("delete_data, object is not array, struct_name:%s", db_struct->struct_name().c_str());
		return;
	}

	Local<Context> context(isolate->GetCurrentContext());
	v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(object);
	uint16_t len = array->Length();
	for (uint i = 0; i < len; ++i) {
		v8::Local<v8::Value> value = array->Get(context, i).ToLocalChecked();
		int64_t key_index = value->NumberValue(context).FromJust();
		get_connection(db_id).remove(db_struct->table_name(), MONGO_QUERY(db_struct->index_name() << (long long int)(key_index)));
		DATA_MANAGER->delete_db_data(db_id, db_struct, key_index);
	}
}

v8::Local<v8::Object> Mongo_Operator::load_data_single(DB_Struct *db_struct, Isolate* isolate, BSONObj &bsonobj) {
	EscapableHandleScope handle_scope(isolate);
	Local<Context> context(isolate->GetCurrentContext());

	Local<ObjectTemplate> localTemplate = ObjectTemplate::New(isolate);
	v8::Local<v8::Object> data_obj = localTemplate->NewInstance(context).ToLocalChecked();
	for(std::vector<Field_Info>::const_iterator iter = db_struct->field_vec().begin();
			iter != db_struct->field_vec().end(); ++iter) {
		v8::Local<v8::String> key = String::NewFromUtf8(isolate, iter->field_name.c_str(), NewStringType::kNormal).ToLocalChecked();
		if(iter->field_label == "arg") {
			v8::Local<v8::Value> value = load_data_arg(db_struct, isolate, *iter, bsonobj);
			if (!value.IsEmpty()) {
				data_obj->Set(context, key, value).FromJust();
			}
		}
		else if(iter->field_label == "vector") {
			v8::Local<v8::Array> array = load_data_vector(db_struct, isolate, *iter, bsonobj);
			if (!array.IsEmpty()) {
				data_obj->Set(context, key, array).FromJust();
			}
		}
		else if(iter->field_label == "map") {
			v8::Local<v8::Map> map = load_data_map(db_struct, isolate, *iter, bsonobj);
			if (!map.IsEmpty()) {
				data_obj->Set(context, key, map).FromJust();
			}
		}
		else if(iter->field_label == "struct") {
			v8::Local<v8::Object> object = load_data_struct(db_struct, isolate, *iter, bsonobj);
			if (!object.IsEmpty()) {
				data_obj->Set(context, key, object).FromJust();
			}
		}
	}
	return handle_scope.Escape(data_obj);
}

v8::Local<v8::Value> Mongo_Operator::load_data_arg(DB_Struct *db_struct, Isolate* isolate, const Field_Info &field_info, BSONObj &bsonobj) {
	EscapableHandleScope handle_scope(isolate);

	Local<Value> value;
	if(field_info.field_type == "int8") {
		int8_t val = bsonobj[field_info.field_name].numberInt();
		value = Int32::New(isolate, val);
	}
	else if(field_info.field_type == "int16") {
		int16_t val = bsonobj[field_info.field_name].numberInt();
		value = Int32::New(isolate, val);
	}
	else if(field_info.field_type == "int32") {
		int32_t val = bsonobj[field_info.field_name].numberInt();
		value = Int32::New(isolate, val);
	}
	else if(field_info.field_type == "int64") {
		int64_t val = bsonobj[field_info.field_name].numberLong();
		value = Number::New(isolate, val);
	}
	else if(field_info.field_type == "uint8") {
		uint8_t val = bsonobj[field_info.field_name].numberInt();
		value = Uint32::New(isolate, val);
	}
	else if(field_info.field_type == "uint16") {
		uint16_t val = bsonobj[field_info.field_name].numberInt();
		value = Uint32::New(isolate, val);
	}
	else if(field_info.field_type == "uint32") {
		uint32_t val = bsonobj[field_info.field_name].numberInt();
		value = Uint32::New(isolate, val);
	}
	else if(field_info.field_type == "uint64") {
		int64_t val = bsonobj[field_info.field_name].numberLong();
		value = Number::New(isolate, val);
	}
	else if(field_info.field_type == "double") {
		double val = bsonobj[field_info.field_name].numberDouble();
		value = Number::New(isolate, val);
	}
	else if(field_info.field_type == "bool") {
		bool val = bsonobj[field_info.field_name].booleanSafe();
		value = Boolean::New(isolate, val);
	}
	else if(field_info.field_type == "string") {
		std::string val = bsonobj[field_info.field_name].valuestrsafe();
		value = String::NewFromUtf8(isolate, val.c_str(), NewStringType::kNormal).ToLocalChecked();
	}
	else {
		LOG_ERROR("Can not find the field_type:%s, field_name:%s, struct_name:%s",
				field_info.field_type.c_str(), field_info.field_name.c_str(), db_struct->struct_name().c_str());
		return handle_scope.Escape(Local<Value>());
	}

	return handle_scope.Escape(value);
}

v8::Local<v8::Array> Mongo_Operator::load_data_vector(DB_Struct *db_struct, Isolate* isolate, const Field_Info &field_info, BSONObj &bsonobj) {
	EscapableHandleScope handle_scope(isolate);
	Local<Context> context(isolate->GetCurrentContext());

	BSONObj fieldobj = bsonobj.getObjectField(field_info.field_name);
	int len = fieldobj.nFields();
	BSONObjIterator field_iter(fieldobj);
	BSONObj obj;

	v8::Local<v8::Array> array = Array::New(isolate, len);
	if(db_struct->is_struct(field_info.field_type)) {
		for(int i = 0; i < len; ++i) {
			obj = field_iter.next().embeddedObject();
			v8::Local<v8::Object> object = load_data_struct(db_struct, isolate, field_info, obj);
			if (!object.IsEmpty()) {
				array->Set(context, i, object).FromJust();
			}
		}
	}
	else {
		for(int i = 0; i < len; ++i) {
			obj = field_iter.next().embeddedObject();
			v8::Local<v8::Value> value = load_data_arg(db_struct, isolate, field_info, obj);
			if (!value.IsEmpty()) {
				array->Set(context, i, value).FromJust();
			}
		}
	}

	return handle_scope.Escape(array);
}

v8::Local<v8::Map> Mongo_Operator::load_data_map(DB_Struct *db_struct, Isolate* isolate, const Field_Info &field_info, BSONObj &bsonobj) {
	EscapableHandleScope handle_scope(isolate);
	Local<Context> context(isolate->GetCurrentContext());

	BSONObj fieldobj = bsonobj.getObjectField(field_info.field_name);
	int len = fieldobj.nFields();
	BSONObjIterator field_iter(fieldobj);
	BSONObj obj;

	v8::Local<v8::Map> map = Map::New(isolate);
	if(db_struct->is_struct(field_info.field_type)) {
		for(int i = 0; i < len; ++i) {
			obj = field_iter.next().embeddedObject();
			v8::Local<v8::Object> object = load_data_struct(db_struct, isolate, field_info, obj);
			v8::Local<v8::Value> key = object->Get(context, String::NewFromUtf8(isolate, field_info.key_name.c_str(), NewStringType::kNormal).ToLocalChecked()).ToLocalChecked();
			if (!key.IsEmpty() && !object.IsEmpty()) {
				map->Set(context, key, object).ToLocalChecked();
			}
		}
	}
	else {
		for(int i = 0; i < len; ++i) {
			obj = field_iter.next().embeddedObject();
			Field_Info key_info;
			key_info.field_label = "arg";
			key_info.field_type = field_info.key_type;
			key_info.field_name = field_info.key_name;
			v8::Local<v8::Value> key = load_data_arg(db_struct, isolate, key_info, obj);
			v8::Local<v8::Value> value = load_data_arg(db_struct, isolate, field_info, obj);
			if (!key.IsEmpty() && !value.IsEmpty()) {
				map->Set(context, key, value).ToLocalChecked();
			}
		}
	}

	return handle_scope.Escape(map);
}

v8::Local<v8::Object> Mongo_Operator::load_data_struct(DB_Struct *db_struct, Isolate* isolate, const Field_Info &field_info, BSONObj &bsonobj) {
	EscapableHandleScope handle_scope(isolate);
	Local<Context> context(isolate->GetCurrentContext());

	DB_Struct *sub_struct = STRUCT_MANAGER->get_db_struct(field_info.field_type);
	if(db_struct == nullptr){
		return handle_scope.Escape(v8::Local<v8::Object>());
	}

	Local<ObjectTemplate> localTemplate = ObjectTemplate::New(isolate);
	v8::Local<v8::Object> object = localTemplate->NewInstance(context).ToLocalChecked();
	BSONObj fieldobj = bsonobj.getObjectField(field_info.field_type);

	std::vector<Field_Info> field_vec = sub_struct->field_vec();
	for(std::vector<Field_Info>::const_iterator iter = field_vec.begin();
			iter != field_vec.end(); iter++) {
		v8::Local<v8::String> key = String::NewFromUtf8(isolate, iter->field_name.c_str(), NewStringType::kNormal).ToLocalChecked();
		if(iter->field_label == "arg") {
			v8::Local<v8::Value> value = load_data_arg(db_struct, isolate, *iter, fieldobj);
			if (!value.IsEmpty()) {
				object->Set(context, key, value).FromJust();
			}
		}
		else if(iter->field_label == "vector") {
			v8::Local<v8::Array> array = load_data_vector(db_struct, isolate, *iter, fieldobj);
			if (!array.IsEmpty()) {
				object->Set(context, key, array).FromJust();
			}
		}
		else if(iter->field_label == "map") {
			v8::Local<v8::Map> map = load_data_map(db_struct, isolate, *iter, fieldobj);
			if (!map.IsEmpty()) {
				object->Set(context, key, map).FromJust();
			}
		}
		else if(iter->field_label == "struct") {
			v8::Local<v8::Object> sub_object = load_data_struct(db_struct, isolate, *iter, fieldobj);
			if (!sub_object.IsEmpty()) {
				object->Set(context, key, sub_object).FromJust();
			}
		}
	}

	return handle_scope.Escape(object);
}

void Mongo_Operator::save_data_arg(DB_Struct *db_struct, Isolate* isolate, const Field_Info &field_info, BSONObjBuilder &builder, v8::Local<v8::Value> value) {
	Local<Context> context(isolate->GetCurrentContext());
	if(field_info.field_type == "int8") {
		int8_t val = 0;
		if (value->IsInt32()) {
			val = value->Int32Value(context).FromJust();
		}
		builder << field_info.field_name << (int)val;
	}
	else if(field_info.field_type == "int16") {
		int16_t val = 0;
		if (value->IsInt32()) {
			val = value->Int32Value(context).FromJust();
		}
		builder << field_info.field_name << (int)val;
	}
	else if(field_info.field_type == "int32") {
		int32_t val = 0;
		if (value->IsInt32()) {
			val = value->Int32Value(context).FromJust();
		}
		builder << field_info.field_name << (int)val;
	}
	else if(field_info.field_type == "int64") {
		int64_t val = 0;
		if (value->IsNumber()) {
			val = value->NumberValue(context).FromJust();
		}
		builder << field_info.field_name << (long long int)val;
	}
	else if(field_info.field_type == "uint8") {
		uint8_t val = 0;
		if (value->IsUint32()) {
			val = value->Uint32Value(context).FromJust();
		}
		builder << field_info.field_name << (uint)val;
	}
	else if(field_info.field_type == "uint16") {
		uint16_t val = 0;
		if (value->IsUint32()) {
			val = value->Uint32Value(context).FromJust();
		}
		builder << field_info.field_name << (uint)val;
	}
	else if(field_info.field_type == "uint32") {
		uint32_t val = 0;
		if (value->IsUint32()) {
			val = value->Uint32Value(context).FromJust();
		}
		builder << field_info.field_name << (uint)val;
	}
	else if(field_info.field_type == "uint64") {
		uint64_t val = 0;
		if (value->IsNumber()) {
			val = value->NumberValue(context).FromJust();
		}
		builder << field_info.field_name << (long long int)val;
	}
	else if(field_info.field_type == "double") {
		double val = 0;
		if (value->IsNumber()) {
			val = value->NumberValue(context).FromJust();
		}
		builder << field_info.field_name << val;
	}
	else if(field_info.field_type == "bool") {
		bool val = 0;
		if (value->IsBoolean()) {
			val = value->BooleanValue(context).FromJust();
		}
		builder << field_info.field_name << val;
	}
	else if(field_info.field_type == "string") {
		std::string val = "";
		if (value->IsString()) {
			String::Utf8Value str(value->ToString(context).ToLocalChecked());
			val = to_string(str);
		}
		builder << field_info.field_name << val;
	}
	else {
		LOG_ERROR("Can not find the field_type:%s, field_name:%s, struct_name:%s",
				field_info.field_name.c_str(), field_info.field_type.c_str(), db_struct->struct_name().c_str());
	}
}

void Mongo_Operator::save_data_vector(DB_Struct *db_struct, Isolate* isolate, const Field_Info &field_info, BSONObjBuilder &builder, v8::Local<v8::Value> value) {
	std::vector<BSONObj> bson_vec;

	if (!value->IsArray()) {
		LOG_ERROR("field_type:%s, field_name:%s is not array, struct_name:%s", field_info.field_type.c_str(), field_info.field_name.c_str(), db_struct->struct_name().c_str());
		builder << field_info.field_name << bson_vec;
		return;
	}

	Local<Context> context(isolate->GetCurrentContext());
	v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(value);
	int len = array->Length();
	for (int i = 0; i < len; ++i) {
		Local<Value> element = array->Get(context, i).ToLocalChecked();
		if(db_struct->is_struct(field_info.field_type)) {
			BSONObjBuilder obj_builder;
			save_data_struct(db_struct, isolate, field_info, obj_builder, element);
			bson_vec.push_back(obj_builder.obj());
		}
		else {
			BSONObjBuilder obj_builder;
			save_data_arg(db_struct, isolate, field_info, obj_builder, element);
			bson_vec.push_back(obj_builder.obj());
		}
	}

	builder << field_info.field_name << bson_vec;
}

void Mongo_Operator::save_data_map(DB_Struct *db_struct, Isolate* isolate, const Field_Info &field_info, BSONObjBuilder &builder, v8::Local<v8::Value> value) {
	std::vector<BSONObj> bson_vec;
	if (!value->IsMap()) {
		LOG_ERROR("field_type:%s, field_name:%s is not map, struct_name:%s", field_info.field_type.c_str(), field_info.field_name.c_str(), db_struct->struct_name().c_str());
		builder << field_info.field_name << bson_vec;
		return;
	}

	Local<Context> context(isolate->GetCurrentContext());
	Local<Map> map = Local<Map>::Cast(value);
	int len = map->Size();
	v8::Local<v8::Array> array = map->AsArray();
	//index N is the Nth key and index N + 1 is the Nth value.
	if(db_struct->is_struct(field_info.field_type)){
		for (int i = 0; i < len * 2; i = i + 2) {
			BSONObjBuilder obj_builder;
			Local<Value> element = array->Get(context, i + 1).ToLocalChecked();
			save_data_struct(db_struct, isolate, field_info, obj_builder, element);
			bson_vec.push_back(obj_builder.obj());
		}
	}
	else {
		Field_Info key_info;
		key_info.field_label = "arg";
		key_info.field_type = field_info.key_type;
		key_info.field_name = field_info.key_name;
		for (int i = 0; i < len * 2; i = i + 2) {
			BSONObjBuilder obj_builder;
			Local<Value> key = array->Get(context, i).ToLocalChecked();
			Local<Value> element = array->Get(context, i + 1).ToLocalChecked();
			save_data_arg(db_struct, isolate, key_info, obj_builder, key);
			save_data_arg(db_struct, isolate, field_info, obj_builder, element);
			bson_vec.push_back(obj_builder.obj());
		}
	}

	builder << field_info.field_name << bson_vec;
}

void Mongo_Operator::save_data_struct(DB_Struct *db_struct, Isolate* isolate, const Field_Info &field_info, BSONObjBuilder &builder, v8::Local<v8::Value> value) {
	if (!value->IsObject()) {
		LOG_ERROR("field_type:%s, field_name:%s is not object, struct_name:%s", field_info.field_type.c_str(), field_info.field_name.c_str(), db_struct->struct_name().c_str());
		return;
	}

	DB_Struct *sub_struct = STRUCT_MANAGER->get_db_struct(field_info.field_type);
	if (sub_struct == nullptr) {
		return;
	}

	Local<Context> context(isolate->GetCurrentContext());
	v8::Local<v8::Object> object = value->ToObject(context).ToLocalChecked();
	BSONObjBuilder obj_builder;
	std::vector<Field_Info> field_vec = sub_struct->field_vec();
	for(std::vector<Field_Info>::const_iterator iter = field_vec.begin();
			iter != field_vec.end(); iter++) {
		v8::Local<v8::Value> element = object->Get(context, String::NewFromUtf8(isolate, iter->field_name.c_str(), NewStringType::kNormal).ToLocalChecked()).ToLocalChecked();
		if(iter->field_label == "arg") {
			save_data_arg(db_struct, isolate, *iter, obj_builder, element);
		}
		else if(iter->field_label == "vector") {
			save_data_vector(db_struct, isolate, *iter, obj_builder, element);
		}
		else if(iter->field_label == "map") {
			save_data_map(db_struct, isolate, *iter, obj_builder, element);
		}
		else if(iter->field_label == "struct") {
			save_data_struct(db_struct, isolate, *iter, obj_builder, element);
		}
	}

	builder << field_info.field_name << obj_builder.obj();
}

void Mongo_Operator::load_data_single(DB_Struct *db_struct, BSONObj &bsonobj, Byte_Buffer &buffer) {
	for(std::vector<Field_Info>::const_iterator iter = db_struct->field_vec().begin();
			iter != db_struct->field_vec().end(); ++iter) {
		if(iter->field_label == "arg") {
			load_data_arg(db_struct, *iter, bsonobj, buffer);
		}
		else if(iter->field_label == "vector" || iter->field_label == "map") {
			load_data_vector(db_struct, *iter, bsonobj, buffer);
		}
		else if(iter->field_label == "struct") {
			load_data_struct(db_struct, *iter, bsonobj, buffer);
		}
	}
}

void Mongo_Operator::load_data_arg(DB_Struct *db_struct, const Field_Info &field_info, BSONObj &bsonobj, Byte_Buffer &buffer) {
	if(field_info.field_type == "int8") {
		int8_t val = bsonobj[field_info.field_name].numberInt();
		buffer.write_int8(val);
	}
	else if(field_info.field_type == "int16") {
		int16_t val = bsonobj[field_info.field_name].numberInt();
		buffer.write_int16(val);
	}
	else if(field_info.field_type == "int32") {
		int32_t val = bsonobj[field_info.field_name].numberInt();
		buffer.write_int32(val);
	}
	else if(field_info.field_type == "int64") {
		int64_t val = bsonobj[field_info.field_name].numberLong();
		buffer.write_int64(val);
	}
	else if(field_info.field_type == "uint8") {
		uint8_t val = bsonobj[field_info.field_name].numberInt();
		buffer.write_uint8(val);
	}
	else if(field_info.field_type == "uint16") {
		uint16_t val = bsonobj[field_info.field_name].numberInt();
		buffer.write_uint16(val);
	}
	else if(field_info.field_type == "uint32") {
		uint32_t val = bsonobj[field_info.field_name].numberInt();
		buffer.write_uint32(val);
	}
	else if(field_info.field_type == "uint64") {
		int64_t val = bsonobj[field_info.field_name].numberLong();
		buffer.write_uint64(val);
	}
	else if(field_info.field_type == "double") {
		double val = bsonobj[field_info.field_name].numberDouble();
		buffer.write_double(val);
	}
	else if(field_info.field_type == "bool") {
		bool val = bsonobj[field_info.field_name].booleanSafe();
		buffer.write_bool(val);
	}
	else if(field_info.field_type == "string") {
		std::string val = bsonobj[field_info.field_name].valuestrsafe();
		buffer.write_string(val);
	}
	else {
		LOG_ERROR("Can not find the field_type:%s, field_name:%s, struct_name:%s",
				field_info.field_type.c_str(), field_info.field_name.c_str(), db_struct->struct_name().c_str());
	}
}

void Mongo_Operator::load_data_vector(DB_Struct *db_struct, const Field_Info &field_info, BSONObj &bsonobj, Byte_Buffer &buffer) {
	BSONObj fieldobj = bsonobj.getObjectField(field_info.field_name);
	uint16_t len = fieldobj.nFields();
	BSONObjIterator field_iter(fieldobj);
	BSONObj obj;

	buffer.write_uint16(len);
	if(db_struct->is_struct(field_info.field_type)) {
		for(int i = 0; i < len; ++i) {
			obj = field_iter.next().embeddedObject();
			load_data_struct(db_struct, field_info, obj, buffer);
		}
	}
	else {
		for(int i = 0; i < len; ++i) {
			obj = field_iter.next().embeddedObject();
			load_data_arg(db_struct, field_info, obj, buffer);
		}
	}
}

void Mongo_Operator::load_data_struct(DB_Struct *db_struct, const Field_Info &field_info, BSONObj &bsonobj, Byte_Buffer &buffer) {
	DB_Struct *sub_struct = STRUCT_MANAGER->get_db_struct(field_info.field_type);
	if(db_struct == nullptr){
		return;
	}

	BSONObj fieldobj = bsonobj.getObjectField(field_info.field_type);
	std::vector<Field_Info> field_vec = sub_struct->field_vec();
	for(std::vector<Field_Info>::const_iterator iter = field_vec.begin();
			iter != field_vec.end(); iter++) {
		if(iter->field_label == "arg") {
			load_data_arg(db_struct, *iter, fieldobj, buffer);
		}
		else if(iter->field_label == "vector" || iter->field_label == "map") {
			load_data_vector(db_struct, *iter, fieldobj, buffer);
		}
		else if(iter->field_label == "struct") {
			load_data_struct(db_struct, *iter, fieldobj, buffer);
		}
	}
}

void Mongo_Operator::save_data_arg(DB_Struct *db_struct, const Field_Info &field_info, BSONObjBuilder &builder, Byte_Buffer &buffer) {
	if(field_info.field_type == "int8") {
		int8_t val = 0;
		buffer.read_int8(val);
		builder << field_info.field_name << (int)val;
	}
	else if(field_info.field_type == "int16") {
		int16_t val = 0;
		buffer.read_int16(val);
		builder << field_info.field_name << (int)val;
	}
	else if(field_info.field_type == "int32") {
		int32_t val = 0;
		buffer.read_int32(val);
		builder << field_info.field_name << (int)val;
	}
	else if(field_info.field_type == "int64") {
		int64_t val = 0;
		buffer.read_int64(val);
		builder << field_info.field_name << (long long int)val;
	}
	else if(field_info.field_type == "uint8") {
		uint8_t val = 0;
		buffer.read_uint8(val);
		builder << field_info.field_name << (uint)val;
	}
	else if(field_info.field_type == "uint16") {
		uint16_t val = 0;
		buffer.read_uint16(val);
		builder << field_info.field_name << (uint)val;
	}
	else if(field_info.field_type == "uint32") {
		uint32_t val = 0;
		buffer.read_uint32(val);
		builder << field_info.field_name << (uint)val;
	}
	else if(field_info.field_type == "uint64") {
		uint64_t val = 0;
		buffer.read_uint64(val);
		builder << field_info.field_name << (long long int)val;
	}
	else if(field_info.field_type == "double") {
		double val = 0;
		buffer.read_double(val);
		builder << field_info.field_name << val;
	}
	else if(field_info.field_type == "bool") {
		bool val = 0;
		buffer.read_bool(val);
		builder << field_info.field_name << val;
	}
	else if(field_info.field_type == "string") {
		std::string val = "";
		buffer.read_string(val);
		builder << field_info.field_name << val;
	}
	else {
		LOG_ERROR("Can not find the field_type:%s, field_name:%s, struct_name:%s",
				field_info.field_type.c_str(), field_info.field_name.c_str(), db_struct->struct_name().c_str());
	}
}

void Mongo_Operator::save_data_vector(DB_Struct *db_struct, const Field_Info &field_info, BSONObjBuilder &builder, Byte_Buffer &buffer) {
	std::vector<BSONObj> bson_vec;

	uint16_t len = 0;
	buffer.read_uint16(len);
	for (uint i = 0; i < len; ++i) {
		if(db_struct->is_struct(field_info.field_type)) {
			BSONObjBuilder obj_builder;
			save_data_struct(db_struct, field_info, obj_builder, buffer);
			bson_vec.push_back(obj_builder.obj());
		}
		else {
			BSONObjBuilder obj_builder;
			save_data_arg(db_struct, field_info, obj_builder, buffer);
			bson_vec.push_back(obj_builder.obj());
		}
	}

	builder << field_info.field_name << bson_vec;
}

void Mongo_Operator::save_data_struct(DB_Struct *db_struct, const Field_Info &field_info, BSONObjBuilder &builder, Byte_Buffer &buffer) {
	DB_Struct *sub_struct = STRUCT_MANAGER->get_db_struct(field_info.field_type);
	if (sub_struct == nullptr) {
		return;
	}

	BSONObjBuilder obj_builder;
	std::vector<Field_Info> field_vec = sub_struct->field_vec();
	for(std::vector<Field_Info>::const_iterator iter = field_vec.begin();
			iter != field_vec.end(); iter++) {
		if(iter->field_label == "arg") {
			save_data_arg(db_struct, *iter, obj_builder, buffer);
		}
		else if(iter->field_label == "vector" || iter->field_label == "map") {
			save_data_vector(db_struct, *iter, obj_builder, buffer);
		}
		else if(iter->field_label == "struct") {
			save_data_struct(db_struct, *iter, obj_builder, buffer);
		}
	}

	builder << field_info.field_name << obj_builder.obj();
}

#endif
