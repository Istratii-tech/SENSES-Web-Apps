package arcwasm;

import android.util.Xml;
import java.io.ByteArrayInputStream;
import java.io.StringReader;
import java.io.StringWriter;
import java.util.ArrayList;
import java.util.List;
import javax.xml.parsers.DocumentBuilderFactory;
import javax.xml.parsers.SAXParser;
import javax.xml.parsers.SAXParserFactory;
import javax.xml.transform.OutputKeys;
import javax.xml.transform.Transformer;
import javax.xml.transform.TransformerFactory;
import javax.xml.transform.dom.DOMSource;
import javax.xml.transform.stream.StreamResult;
import org.w3c.dom.Document;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;
import org.xml.sax.Attributes;
import org.xml.sax.InputSource;
import org.xml.sax.SAXParseException;
import org.xml.sax.XMLReader;
import org.xml.sax.helpers.DefaultHandler;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserFactory;
import org.xmlpull.v1.XmlSerializer;

/**
 * XML on Android 6.0.1: SAX through libcore's ExpatParser (expat, the
 * natives of libjavacore), XmlPullParser (kxml), DOM, javax.xml.transform
 * (apache-xml) and android.util.Xml. Prints one line per check and exits
 * with the number of failures.
 */
public class XmlTest {
    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static void expectEq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
        }
    }

    static final String DOC =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            + "<!DOCTYPE feed [<!ENTITY who \"мир\">]>\n"
            + "<feed xmlns=\"urn:a\" xmlns:b=\"urn:b\" lang=\"ru\">"
            + "<item id=\"1\" b:rank=\"5\">Hello, &who;! &amp; &#x41;&#66;</item>"
            + "<b:item id=\"2\"><![CDATA[<raw> & text]]></b:item>"
            + "<empty/>"
            + "</feed>";

    /** Records SAX events as text lines. */
    static class Recorder extends DefaultHandler {
        final List<String> events = new ArrayList<String>();
        final StringBuilder text = new StringBuilder();

        private void flush() {
            if (text.length() > 0) {
                events.add("text " + text);
                text.setLength(0);
            }
        }

        @Override public void startElement(String uri, String local, String qName, Attributes a) {
            flush();
            StringBuilder sb = new StringBuilder("start {" + uri + "}" + local);
            for (int i = 0; i < a.getLength(); i++) {
                sb.append(" {").append(a.getURI(i)).append("}").append(a.getLocalName(i))
                        .append("=").append(a.getValue(i));
            }
            events.add(sb.toString());
        }

        @Override public void endElement(String uri, String local, String qName) {
            flush();
            events.add("end " + local);
        }

        @Override public void characters(char[] ch, int start, int length) {
            text.append(ch, start, length);
        }
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();

        check("SAX (ExpatParser): namespaces, attributes, entities, CDATA, UTF-8", new Check() {
            public void run() throws Exception {
                SAXParserFactory f = SAXParserFactory.newInstance();
                f.setNamespaceAware(true);
                SAXParser p = f.newSAXParser();
                XMLReader r = p.getXMLReader();
                expectEq("org.apache.harmony.xml.ExpatReader", r.getClass().getName());
                Recorder rec = new Recorder();
                p.parse(new ByteArrayInputStream(DOC.getBytes("UTF-8")), rec);
                List<String> want = new ArrayList<String>();
                want.add("start {urn:a}feed {}lang=ru");
                want.add("start {urn:a}item {}id=1 {urn:b}rank=5");
                want.add("text Hello, мир! & AB");
                want.add("end item");
                want.add("start {urn:b}item {}id=2");
                want.add("text <raw> & text");
                want.add("end item");
                want.add("start {urn:a}empty");
                want.add("end empty");
                want.add("end feed");
                expectEq(want, rec.events);
            }
        });

        check("SAX (ExpatParser): malformed document -> SAXParseException with its line", new Check() {
            public void run() throws Exception {
                try {
                    SAXParserFactory.newInstance().newSAXParser().parse(
                            new InputSource(new StringReader("<a>\n<b>\n</a>")), new DefaultHandler());
                    throw new AssertionError("no exception");
                } catch (SAXParseException e) {
                    expectEq(Integer.valueOf(3), Integer.valueOf(e.getLineNumber()));
                }
            }
        });

        check("SAX (ExpatParser): 20 000 elements from a stream", new Check() {
            public void run() throws Exception {
                StringBuilder sb = new StringBuilder("<list>");
                for (int i = 0; i < 20000; i++) {
                    sb.append("<e n=\"").append(i).append("\">ж").append(i).append("</e>");
                }
                sb.append("</list>");
                final int[] count = new int[1];
                final long[] sum = new long[1];
                SAXParserFactory.newInstance().newSAXParser().parse(
                        new ByteArrayInputStream(sb.toString().getBytes("UTF-8")), new DefaultHandler() {
                            @Override public void startElement(String uri, String local, String q, Attributes a) {
                                if ("e".equals(q)) {
                                    count[0]++;
                                    sum[0] += Integer.parseInt(a.getValue("n"));
                                }
                            }
                        });
                expectEq(Integer.valueOf(20000), Integer.valueOf(count[0]));
                expectEq(Long.valueOf(19999L * 20000 / 2), Long.valueOf(sum[0]));
            }
        });

        check("XmlPullParser (kxml): events, namespaces, attributes, text", new Check() {
            public void run() throws Exception {
                XmlPullParserFactory f = XmlPullParserFactory.newInstance();
                f.setNamespaceAware(true);
                XmlPullParser p = f.newPullParser();
                p.setInput(new StringReader(
                        "<r xmlns:x=\"urn:x\"><x:a k=\"v\">t1</x:a><b>t2<c/></b></r>"));
                StringBuilder sb = new StringBuilder();
                for (int ev = p.getEventType(); ev != XmlPullParser.END_DOCUMENT; ev = p.next()) {
                    if (ev == XmlPullParser.START_TAG) {
                        sb.append('<').append(p.getNamespace()).append(':').append(p.getName());
                        for (int i = 0; i < p.getAttributeCount(); i++) {
                            sb.append(' ').append(p.getAttributeName(i)).append('=')
                                    .append(p.getAttributeValue(i));
                        }
                        sb.append('>');
                    } else if (ev == XmlPullParser.END_TAG) {
                        sb.append("</").append(p.getName()).append('>');
                    } else if (ev == XmlPullParser.TEXT) {
                        sb.append(p.getText());
                    }
                }
                expectEq("<:r><urn:x:a k=v>t1</a><:b>t2<:c></c></b></r>", sb.toString());
            }
        });

        check("DOM: DocumentBuilder, getElementsByTagNameNS, Transformer (apache-xml)", new Check() {
            public void run() throws Exception {
                DocumentBuilderFactory f = DocumentBuilderFactory.newInstance();
                f.setNamespaceAware(true);
                Document d = f.newDocumentBuilder().parse(
                        new ByteArrayInputStream("<a xmlns=\"urn:a\"><i n=\"1\"/><i n=\"2\">x</i></a>".getBytes("UTF-8")));
                NodeList items = d.getElementsByTagNameNS("urn:a", "i");
                expectEq(Integer.valueOf(2), Integer.valueOf(items.getLength()));
                expectEq("x", ((Element) items.item(1)).getTextContent());
                Element added = d.createElementNS("urn:a", "i");
                added.setAttribute("n", "3");
                d.getDocumentElement().appendChild(added);
                Transformer t = TransformerFactory.newInstance().newTransformer();
                t.setOutputProperty(OutputKeys.OMIT_XML_DECLARATION, "yes");
                StringWriter out = new StringWriter();
                t.transform(new DOMSource(d), new StreamResult(out));
                expectEq("<a xmlns=\"urn:a\"><i n=\"1\"/><i n=\"2\">x</i><i n=\"3\"/></a>", out.toString());
            }
        });

        check("android.util.Xml: parse (expat), newPullParser, newSerializer", new Check() {
            public void run() throws Exception {
                Recorder rec = new Recorder();
                Xml.parse("<a x=\"1\">été</a>", rec);
                expectEq("[start {}a {}x=1, text été, end a]", rec.events.toString());

                XmlSerializer s = Xml.newSerializer();
                StringWriter w = new StringWriter();
                s.setOutput(w);
                s.startDocument("UTF-8", Boolean.TRUE);
                s.startTag(null, "m").attribute(null, "k", "a&b").text("<я>").endTag(null, "m");
                s.endDocument();
                String xml = w.toString();
                expect(xml.endsWith("<m k=\"a&amp;b\">&lt;я&gt;</m>"), xml);

                XmlPullParser p = Xml.newPullParser();
                p.setInput(new StringReader(xml));
                expectEq(Integer.valueOf(XmlPullParser.START_TAG), Integer.valueOf(p.nextTag()));
                expectEq("a&b", p.getAttributeValue(null, "k"));
                expectEq("<я>", p.nextText());
            }
        });

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("XmlTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
