#include "bw_graph/service/graph_service.h"
#include "db_monacgraph_jni_BwGraphJNI.h"

#include <bit>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using bw_graph::service::graph_service_t;
graph_service_t& service() {
  static graph_service_t instance;
  return instance;
}
struct pending_exception {};
void raise(JNIEnv* env, const char* type, const char* message) {
  if (env->ExceptionCheck())
    return;
  auto cls = env->FindClass(type);
  if (cls) {
    // ThrowNew requires modified UTF-8, whereas native error paths can contain
    // arbitrary UTF-8/file bytes. Escape them without allocating in an OOM path.
    char safe[2048];
    size_t n = 0;
    constexpr char hex[] = "0123456789abcdef";
    for (auto p = reinterpret_cast<const unsigned char*>(message); *p && n + 4 < sizeof(safe);
         ++p) {
      if (*p >= 32 && *p < 127)
        safe[n++] = static_cast<char>(*p);
      else {
        safe[n++] = '\\';
        safe[n++] = 'x';
        safe[n++] = hex[*p >> 4];
        safe[n++] = hex[*p & 15];
      }
    }
    safe[n] = 0;
    env->ThrowNew(cls, safe);
    env->DeleteLocalRef(cls);
  }
}
template <class Fn>
auto call(JNIEnv* env, Fn fn) -> decltype(fn()) {
  try {
    return fn();
  } catch (const pending_exception&) {
  } // Preserve OOM or other JVM exceptions.
  catch (const std::invalid_argument& e) {
    raise(env, "java/lang/IllegalArgumentException", e.what());
  } catch (const std::out_of_range& e) {
    raise(env, "java/util/NoSuchElementException", e.what());
  } catch (const std::bad_alloc&) {
    raise(env, "java/lang/OutOfMemoryError", "Native allocation failed");
  } catch (const std::exception& e) {
    raise(env, "java/lang/IllegalStateException", e.what());
  } catch (...) {
    raise(env, "java/lang/IllegalStateException", "Unknown native failure");
  }
  return {};
}
std::string bytes(JNIEnv* env, jbyteArray input, size_t limit) {
  if (!input)
    throw std::invalid_argument("Null byte array");
  auto length = env->GetArrayLength(input);
  if (env->ExceptionCheck())
    throw pending_exception{};
  if (size_t(length) > limit)
    throw std::invalid_argument("Byte array exceeds interface limit");
  std::string result(size_t(length), '\0');
  if (length)
    env->GetByteArrayRegion(input, 0, length, reinterpret_cast<jbyte*>(result.data()));
  if (env->ExceptionCheck())
    throw pending_exception{};
  return result;
}
std::string id(JNIEnv* env, jbyteArray value) { return bytes(env, value, 1024 * 1024); }
std::string data(JNIEnv* env, jbyteArray value) { return bytes(env, value, 16 * 1024 * 1024); }
jbyteArray array(JNIEnv* env, const std::string& value) {
  if (value.size() > size_t(std::numeric_limits<jsize>::max()))
    throw std::length_error("Java byte array limit exceeded");
  auto result = env->NewByteArray(static_cast<jsize>(value.size()));
  if (!result)
    throw pending_exception{};
  if (!value.empty())
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(value.size()),
                            reinterpret_cast<const jbyte*>(value.data()));
  if (env->ExceptionCheck())
    throw pending_exception{};
  return result;
}
jlong bits(uint64_t value) { return std::bit_cast<jlong>(value); }
jlongArray array(JNIEnv* env, const std::vector<uint64_t>& value) {
  if (value.size() > size_t(std::numeric_limits<jsize>::max()))
    throw std::length_error("Java long array limit exceeded");
  auto result = env->NewLongArray(static_cast<jsize>(value.size()));
  if (!result)
    throw pending_exception{};
  // Bounded conversion scratch, avoiding aliasing uint64_t as jlong.
  jlong chunk[1024];
  for (size_t offset = 0; offset < value.size(); offset += 1024) {
    const auto count = std::min(size_t(1024), value.size() - offset);
    for (size_t i = 0; i < count; ++i)
      chunk[i] = bits(value[offset + i]);
    env->SetLongArrayRegion(result, static_cast<jsize>(offset), static_cast<jsize>(count), chunk);
    if (env->ExceptionCheck())
      throw pending_exception{};
  }
  return result;
}
} // namespace

// javac -h declarations are included above, checking the JNI signatures at build time.
JNIEXPORT jlong JNICALL Java_db_monacgraph_jni_BwGraphJNI_openNative(JNIEnv* e, jobject,
                                                                     jbyteArray root,
                                                                     jbyteArray name) {
  return call(e, [&] {
    auto path = id(e, root), graph = id(e, name);
    if (path.find('\0') != std::string::npos)
      throw std::invalid_argument("NUL in graph root");
    return bits(service().open(path, graph));
  });
}
JNIEXPORT void JNICALL Java_db_monacgraph_jni_BwGraphJNI_closeDB(JNIEnv* e, jobject, jlong h) {
  call(e, [&] {
    service().close(h);
    return 0;
  });
}
JNIEXPORT jlong JNICALL Java_db_monacgraph_jni_BwGraphJNI_createVertex(JNIEnv* e, jobject, jlong h,
                                                                       jbyteArray outer,
                                                                       jbyteArray value) {
  return call(e, [&] { return bits(service().create_vertex(h, id(e, outer), data(e, value))); });
}
JNIEXPORT jlong JNICALL Java_db_monacgraph_jni_BwGraphJNI_createEdge(JNIEnv* e, jobject, jlong h,
                                                                     jbyteArray outer, jlong src,
                                                                     jlong dst, jbyteArray value) {
  return call(
      e, [&] { return bits(service().create_edge(h, id(e, outer), src, dst, data(e, value))); });
}
#define ENTITY_METHODS(Name, Edge)                                                                 \
  JNIEXPORT jlong JNICALL Java_db_monacgraph_jni_BwGraphJNI_get##Name##HandleById(                 \
      JNIEnv* e, jobject, jlong h, jbyteArray outer) {                                             \
    return call(e, [&]() -> jlong { return service().find(h, Edge, id(e, outer)); });              \
  }                                                                                                \
  JNIEXPORT jbyteArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_getDataFrom##Name##Handle(        \
      JNIEnv* e, jobject, jlong h, jlong v) {                                                      \
    return call(e, [&] { return array(e, service().get(h, Edge, v)); });                           \
  }                                                                                                \
  JNIEXPORT void JNICALL Java_db_monacgraph_jni_BwGraphJNI_put##Name##Data(                        \
      JNIEnv* e, jobject, jlong h, jlong v, jbyteArray value) {                                    \
    call(e, [&] {                                                                                  \
      service().put(h, Edge, v, data(e, value));                                                   \
      return 0;                                                                                    \
    });                                                                                            \
  }                                                                                                \
  JNIEXPORT void JNICALL Java_db_monacgraph_jni_BwGraphJNI_remove##Name(JNIEnv* e, jobject,        \
                                                                        jlong h, jlong v) {        \
    call(e, [&] {                                                                                  \
      service().remove(h, Edge, v);                                                                \
      return 0;                                                                                    \
    });                                                                                            \
  }                                                                                                \
  JNIEXPORT jbyteArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_get##Name##ExternalId(            \
      JNIEnv* e, jobject, jlong h, jlong v) {                                                      \
    return call(e, [&] { return array(e, service().external_id(h, Edge, v)); });                   \
  }
ENTITY_METHODS(Vertex, false)
ENTITY_METHODS(Edge, true)
#undef ENTITY_METHODS
JNIEXPORT jlongArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_getAllVertices(JNIEnv* e, jobject,
                                                                              jlong h) {
  return call(e, [&] { return array(e, service().all(h, false)); });
}
JNIEXPORT jlongArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_getAllEdges(JNIEnv* e, jobject,
                                                                           jlong h) {
  return call(e, [&] { return array(e, service().all(h, true)); });
}
JNIEXPORT jlongArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_getEdgeHandleByVertex(
    JNIEnv* e, jobject, jlong h, jlong v, jint d) {
  return call(e, [&] { return array(e, service().incident(h, v, d)); });
}
JNIEXPORT jlongArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_getBfsVertices(JNIEnv* e, jobject,
                                                                              jlong h, jlong v) {
  return call(e, [&] { return array(e, service().bfs(h, v)); });
}
JNIEXPORT jlongArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_getWCC(JNIEnv* e, jobject, jlong h) {
  return call(e, [&] { return array(e, service().components(h, false)); });
}
JNIEXPORT jlongArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_getSCC(JNIEnv* e, jobject, jlong h) {
  return call(e, [&] { return array(e, service().components(h, true)); });
}

JNIEXPORT jlongArray JNICALL Java_db_monacgraph_jni_BwGraphJNI_getCommunities(JNIEnv* e, jobject,
                                                                              jlong h) {
  return call(e, [&] { return array(e, service().communities(h)); });
}
