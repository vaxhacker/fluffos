promise http_request(string, mapping | void);

// Reserved design, not implemented yet (continuous CouchDB _changes feeds):
// int http_stream(string url, mapping opts, function on_chunk, function on_close);
// int http_stream_close(int handle);
