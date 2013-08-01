//
// LibSourcey
// Copyright (C) 2005, Sourcey <http://sourcey.com>
//
// LibSourcey is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// LibSourcey is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//


#ifndef SOURCEY_JSON_H
#define SOURCEY_JSON_H


#include "Sourcey/Types.h"
#include "Sourcey/Exception.h"
#include "json/json.h"
#include <fstream>


namespace scy {
namespace json {


// check https://code.google.com/p/rapidjson/

	
typedef Json::Value Value;
typedef Json::ValueIterator ValueIterator;
typedef Json::StyledWriter StyledWriter;
typedef Json::FastWriter FastWriter;
typedef Json::Reader Reader;

	
inline void loadFile(json::Value& root, const std::string& path) 
{
	std::ifstream infile;
	infile.open(path.c_str(), std::ifstream::in);
	if (!infile.is_open())		
		throw FileException("Cannot open input file: " + path);

	json::Reader reader;
	bool res = reader.parse(infile, root);
	infile.close();
	if (!res)
		throw Exception("Invalid JSON format: " + reader.getFormatedErrorMessages());
}


inline void saveFile(const json::Value& root, const std::string& path) 
{
	json::StyledWriter writer;
	std::ofstream outfile;
	outfile.open(path.c_str());
	if (!outfile.is_open())
		throw FileException("Cannot open output file: " + path);

	outfile << writer.write(root);
	outfile.close();
}


inline void stringify(const json::Value& root, std::string& output, bool pretty = false) 
{
	if (pretty) {
		json::StyledWriter writer;
		output = writer.write(root);
	}
	else {
		json::FastWriter writer;
		output = writer.write(root);

		// NOTE: The FastWriter appends a newline
		// character which we don't want.
		if (output.size() > 0)
			output.resize(output.size() - 1);
	}
}


inline std::string stringify(const json::Value& root, bool pretty = false) 
{
	std::string output;
	stringify(root, output, pretty);
	return output;
}


inline void assertMember(const json::Value& root,  const std::string& name) 
{
	if (!root.isMember(name))
		throw Exception("A '" + name + "' member is required.");
}


inline void countNestedKeys(json::Value& root, const std::string& key, int& count, int depth = 0) 
{
	depth++;
	for (json::ValueIterator it = root.begin(); it != root.end(); it++) {
		if ((*it).isObject() && 
			(*it).isMember(key))
			count++;
		countNestedKeys(*it, key, count, depth);
	}
}


inline bool hasNestedKey(json::Value& root, const std::string& key, int depth = 0) 
{
	depth++;
	for (json::ValueIterator it = root.begin(); it != root.end(); it++) {
		if ((*it).isObject() && 
			(*it).isMember(key))
			return true;
		if (hasNestedKey(*it, key, depth))
			return true;
	}
	return false;
}


inline bool findNestedObjectWithProperty(json::Value& root, json::Value*& result, 
	const std::string& key, const std::string& value, 
	bool partial = true, int index = 0, int depth = 0) 
	/// Only works for objects with string values.
	/// Key or value may be empty for selecting wildcard values.
	/// If partial is false substring matches will be accepted.
	/// Result must be a reference to a pointer or the root value's
	/// internal reference will also be changed when the result is 
	/// assigned. Further investigation into jsoncpp is required.
{
	depth++;
	if (root.isObject()) {
		json::Value::Members members = root.getMemberNames();
		for (size_t i = 0; i < members.size(); i++) {		
			json::Value& test = root[members[i]];
			if (test.isNull())
				continue;
			else if (test.isString() && 
				(key.empty() || members[i] == key) &&
				(value.empty() || (!partial ? 
					test.asString() == value :
					test.asString().find(value) != std::string::npos))) {
					if (index == 0) {
						result = &root;
						return true;
					} else
						index--;
			} 
			else if ((test.isObject() || test.isArray()) &&
				findNestedObjectWithProperty(root[members[i]], result, key, value, partial, index, depth))
				return true;
		}
	}
	else if (root.isArray()) {		
		for (size_t i = 0; i < root.size(); i++) {		
			json::Value& test = root[i];
			if (!test.isNull() && (test.isObject() || test.isArray()) &&
				findNestedObjectWithProperty(root[i], result, key, value, partial, index, depth))
				return true;
		}
	}
	return false;
}


} } // namespace scy::json


#endif // SOURCEY_JSON_H