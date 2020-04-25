# ElasticSearch with a SMILE

A short intro - I'm a lead SRE engineer at tinkoff.ru, and we are (like many others) building many interesting products using [ElasticSearch](https://elastic.co) and [OpenDistro](https://opendistro.github.io/for-elasticsearch/) specifically. In fact, I've recently contributed our patches to the upstream of the latter. I've always wanted to write a good ElasticSearch blog post and for a long time but the circumstances and the material were never right - it's was either too complicated, some already well-covered stuff, or plain too specific to a particular environment.

Thankfully, an opportunity came along to report about a small protocol optimization that, on one hand is (almost) fully documented in the official sources, and on the other hand - requires one to jump through a number of hoops to get it. So without further introductions, let's start with the subject matter.

## The romance of ElasticSearch and Content-Type(s)

I've been following the development of ElasticSearch since the days of 1.1, which was the first ElasticSearch I used in production,
even though they skipped a deal of numbers, in that version jump from 2.x->5.x, it's still a long road and it's important to highlight
a bit of history that is directly relevant to the trick I'm about to describe.

While from the begining ElasticSearch was positioned as easily (elastic!) scalable RESTful distributed search engine
with JSON being used for exchange, the team experimented a lot with different APIs (e.g. Thrift API was supported for some time)
and formats by following HTTP and REST principles of content type negatiation.

So with tis in mind it comes as no surprise today that the following command:
```
curl -H 'Accept: application/yaml' localhost:9200
```
produces the well-known text but neatly arranged in a YAML format:
```
---
name: ""
cluster_name: "elasticsearch"
cluster_uuid: "P31eNx8CS2KQK62OU3ek8Q"
version:
  number: "7.6.2"
  build_flavor: "default"
  build_type: "tar"
  build_hash: "ef48eb35cf30adf4db14086e8aabd07ef6fb113f"
  build_date: "2020-03-26T06:34:37.794943Z"
  build_snapshot: false
  lucene_version: "8.4.0"
  minimum_wire_compatibility_version: "6.8.0"
  minimum_index_compatibility_version: "6.0.0-beta1"
tagline: "You Know, for Search"
```

But wait, there is more! ElasticSearch supports not only YAML and JSON, but their binary cousins [CBOR and SMILE](https://www.elastic.co/guide/en/elasticsearch/reference/current/common-options.html#_content_type_requirements). Note the last sentence here - "The bulk and multi-search APIs support NDJSON, JSON, and SMILE; other types will result in an error response". Literally this means that the only binary format supported everywhere equals to JSON is [SMILE](https://github.com/FasterXML/smile-format-specification). That's the reason behind today's post, even though I have discovered the mechanism before fact checking to see if it was properly documented.

Also of possible interest is the fact that ElasticSearch metadata and communication across transport layer is done in SMILE, while keeping the documents themselves in whatever format they were sent in. The idea I assume is that the subsequent search will ask for the same format in response, thus saving on transcoding. What this means is that in order to benefit
from a different format, we'd have to index data in that format, but we'll retain the ability to get (search) responses in any content type we'd ask for via transcoding.

## Working with a SMILE

Let's start with transcoding JSON to SMILE, to produce binary documents for indexing. Thankfully, SMILE was designed by the same folks (FasterXML group) who gave us the famous Jackson library making it a breeze to produce and re-encode SMILE from any other supported format. I'll use Kotlin throughout the blog post with the main goal to keep code as simple as possible but efficient enough to be represenative of production code. Lastly I believe that snippets in the blog posts should be easily portable to any language so I minimize the amount of language-specific constructs.

```kotlin
val from = File("doc.json")
val to = File("doc.smile")
val smile = SmileFactory()
val json = JsonFactory()
val jsonMapper = ObjectMapper(json)
val smileMapper = ObjectMapper(smile)
FileOutputStream(to).use { out ->
    val buffer = ByteArrayOutputStream()
    for (f in from) {
        f.forEachLine { line ->
            buffer.reset()
            smile.createGenerator(buffer).use { gen ->
                smileMapper.writeTree(gen, jsonMapper.readTree(line))
            }
            buffer.write(0xff)
            out.write(buffer.toByteArray())
        }
    }
}
```

The script above is the essense of JSON -> SMILE conversion ([full featured script](https://gist.github.com/DmitryOlshansky/7b1b5a449c559a5253ce4789bba32b86) + [POM](https://gist.github.com/DmitryOlshansky/67e952c2e06fad2d0c9136ad559da4d5)). Finally, we can upload the resulting SMILE documents with `curl` like this:

```shell
# note that this command posts smile content but passes Accepts header for JSON
# as response format to avoid dumping SMILE to the console
curl -XPOST -H 'Content-type: application/smile' localhost:9200/test-index/_doc --data-binary @doc.smile -H 'Accept: application/json'
```

It's tempting to go stright to benchmarking - obviously using binary format instead of text would give us non-zero performance benefits. However there is one last issue to sort out - how to compose [Bulk API](https://www.elastic.co/guide/en/elasticsearch/reference/current/docs-bulk.html) requests in SMILE format. And documentation at the time of writing is lacking, listing only `\n` as separator which doesn't work (I tried it so you don't have to). 

I kept thinking about this missing bit of information for a few days and finally decided to dive into ElasticSearch [source code](https://github.com/elastic/elasticsearch/blob/v7.6.2/server/src/main/java/org/elasticsearch/action/bulk/BulkRequestParser.java) for the answers. Surely enough, the code doesn't reference `\n` constant anywhere and instead points to a format-specific streaming separator value, which happens to be [0xFF](https://github.com/elastic/elasticsearch/blob/master/libs/x-content/src/v7.6.2/java/org/elasticsearch/common/xcontent/smile/SmileXContent.java#L74)
for SMILE.

With that in knowledge in hand we are ready to prepare SMILE-enabled bulk insertion script. The same [full version of the script](https://gist.github.com/DmitryOlshansky/7b1b5a449c559a5253ce4789bba32b86)) is using a thread pool for scheduling a fixed number of parallel inserter threads reading from a queue + a single main thread that splits file by separator value and dispatches batches of documents to the work queue. This proves to be enough to easily saturate CPUs of a small cluster with a single laptop and more than enough for our benchmark setup, as we'll see in the next section.

## Benchmark

Benchmarking is both an art and science, so I'll iterate over aspects of how I obtained the graph below using that script as the main tool. 

First - I tend to use shell script runners on top of existing command-line tools, as it keeps the tool reusable and allows me to play with parameters. The script listed below produces CSV files with raw metrics from elapsed wall-clock time outputted by the Kotlin driver script. 

```shell
threads=10
server=10.0.0.6
echo "size,fmt,time,"
for fmt in json smile ; do
# here we can flexibly define any number of ranges to test for
  for bulk in `seq 100 50 500` `seq 500 25 700` `seq 700 100 1500` ... ; do
  echo -n "$bulk,$fmt"
    for run in `seq 5` ; do
      # the pipline tucked at the end of command extract the digits
      # of total indexing time as reported by elastik script and ends them with ','
      java -Xms1g -Xmx1g -jar elastik.jar upload --threads=10 --size=$bulk http_logs.json http://$server:9200/logs | grep -oP "\\d+ ms" | tr '\n' ',' | sed -r 's/ *ms *//'
    done
  done
  # new line - new row of samples
  echo
done
```

To minimize the effect of noise I plotted the best of 5 runs for each parameter combination. The dataset - 300Mib of http access logs was taken from [ES Rally](https://github.com/elastic/rally) macrobenchmark framework by elastic.co guys.

To further simplify reproduction I use public cloud that I happen to know best - Azure. Nothing should prevent you from using any other cloud to obtain simillar numbers, in fact I run preliminary tests on my laptop getting the same ratio of performance for JSON vs SMILE. 

ElasticSearch server was run using stock configuration from tarball except for setting heap size (8 GiB), network configuration and fixing sysctl `vm_max_map_count=500000` and `memlock=unlimited` ulimits to pass production server bootstrap checks.

Client instance - standard D2s v3 (2 vcpus, 8 GiB).
Average CPU utilization during the benchmark - 20%.

Server instance - standard D4s v3 (4 vcpus, 16 GiB) with 1 Tb of Premium SSD disk (5k IOPs, which is 5x higher than maximum actually used). Average CPU utilization of 75-85% with more utilization correlating with more optimal values of bulk size and usage of SMILE format.


So finally with routine and disclaimers safely out of the way, the picture:

![SMILE vs JSON run-times](./SMILE-vs-JSON.png)

## Discussion

The interesting part is not the 5+% of throughput at all bulk sizes. It's the shape - SMILE gains are larger on both too small and too big size ranges. That is significant and let me briefly explain why.

Grossly overshooting when picking the size of bulk requests, including up to an order of magnitude beyond the right value, is done routinely in many ElasticSearch setups I audited. Picking the right bulk size is an interesting problem and a good topic for its own blog entry, my verdict is that the only way to completly solve it is via dynamic optimization using self-tuning architecture. The fact that SMILE version is much less sensitive to hitting the sweet spot of bulk size helps a lot - what is good size today will change tomorrow, when you are not around to fix it.

Some closing thoughts on where the 5% better CPU utilization are coming from. The bulk of it is likely GC work and pause times being reduced - less text to process and ~20% smaller payloads may translate to less frequent STW pauses. 
Anyhow this is only a guess work for now, as such things are never what they seem and in-depth analysis is outside of the scope of this post. Also not covering the potential saving on the network bandwith, these are obviously nice to have but range in 10-20% of size typically (better for larger documents).

And that's it, that one wierd trick to speed up your bulk indexing by 5+% with modest effort on the side of the ingest application.
