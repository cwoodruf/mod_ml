/**
 * open an ip socket on the given port
 * accept input from mod_ml apache processes
 * the input should be in the form of 
 * {apache host} {ip} 
 * or 
 * {apache host}/{ip}
 * the host/ip pair are looked up in the botstats table
 * if stats info for that host/ip is found then it 
 * is fed to each of the arbitrary number of serialized weka models
 * provided and the most voted for class is then returned as 
 * YES if the ip is believed to be a bot, NO if it is not or 
 * NOTFOUND if there is no record of it
 *
 * a mod_ml preprocessor (using the MLPreprocessor directive) is
 * needed to update the botstats table - see botlogger.py and botlog.py 
 *
 * references:
 * see https://weka.wikispaces.com/Creating+an+ARFF+file
 * also http://crunchify.com/how-to-read-json-object-from-file-in-java/
 */

import java.io.ObjectInputStream;
import java.io.FileInputStream;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.DataOutputStream;
import java.io.DataInputStream;
import java.io.PrintWriter;
import java.io.IOException;

import java.net.ServerSocket;
import java.net.Socket;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.Statement;
import java.sql.PreparedStatement;
import java.sql.SQLException;

import java.util.Map;
import java.util.HashMap;
import java.util.regex.Pattern;
import java.util.regex.Matcher;
import java.util.ArrayList;
import java.util.Iterator;

import weka.core.Attribute;
import weka.core.FastVector;
import weka.core.Instance;
import weka.core.Instances;
import weka.classifiers.Classifier;

/**
 * read a HTTP_HOST and REMOTE_ADDR from the command line
 * look them up in botnormalized and then run a classifier
 * on the "+stats+" data
 *
 * @author is annoyed at how broken weka's java interfaces are
 */
public class BotClassifier {

    public static boolean verbose = false;
    public static String usage = 
        "Usage: java [usual java crap] MLClassifier {port} {stats table} {model files}";

    // database stuff
    private static String dbhost = null;
    private static String dbport = null;
    private static String dbname = null;
    private static String dbuser = null;
    private static String dbpw = null;
    private static String dsn = null;
    private static Connection conn = null;

    // size of results from our query - must match what the models expect
    private static int BOTSTATSZ = 16;
    // needed for making attributes
    private static String[] keys = new String[BOTSTATSZ];

    // table to find data on a particular IP
    // this table must be equivalent to the source from which the models were generated
    private static String stats = "{stats table}"; 
    private static String query = null;

    // we do multiple classifications and vote on the result
    // as a result you'd want an odd number of classifiers
    private static ArrayList<Classifier> classifiers = null;

    private static int port;

    // remember previous classifications
    private static HashMap<String,Judgement> seen = new HashMap<String,Judgement>();

    long reqcount = 0;

    class Judgement {
        private String ip;
        private String judgement;
        private int ttl = 0;

        public Judgement(String ip, String judgement, int ttl) {
            this.ip = ip;
            this.judgement = judgement;
            this.ttl = ttl;
        }
        public String getJudgement() {
            return judgement;
        }
        public String decJudgement() {
            decTtl();
            if (dead()) return null;
            return judgement;
        }
        public String getIp() {
            return ip;
        }
        public int getTtl() {
            return ttl;
        }
        public int decTtl() {
            if (ttl > 0) ttl--;
            return ttl;
        }
        public boolean dead() {
            return (ttl <= 0);
        }
        public String toString() {
            return "ttl "+ttl+" class "+judgement+" for "+ip;
        }
    }

    class Loop extends Thread {
        private Socket client = null;

        public Loop(Socket sock) {
            super("BotClassifier.Loop");
            this.client = sock;
        }

        /**
         * run the conversation between a client and us as a thread
         * get an ip then make a classification and return a YES/NO judgement
         */
        public void run() {
            String rawip = null;
            try (
                    BufferedReader in = new BufferedReader(new InputStreamReader(client.getInputStream()));
                    PrintWriter out = new PrintWriter(new OutputStreamWriter(client.getOutputStream()),true);
            ) {
                while ((rawip = in.readLine()) != null) {
                    String label = passJudgement(getIp(rawip));
                    out.println(label);
                    break;
                }
            } catch (IOException e) {
                System.err.println("IOException in loop: "+e.getMessage());
            }
        }
    }

    /**
     * make an arbitrarily large set of attributes
     * where the first one is a 1,-1 label
     * and the rest are numbers
     */
    public static FastVector mkAtts() {
        FastVector atts = new FastVector();
        if (keys[0] == null) setKeys();

        for (int i=0; i<keys.length; i++) {
            // the first attribute is the class (-1 or 1)
            if (i == 0) {
                FastVector labels = new FastVector();
                labels.addElement("-1");
                labels.addElement("1");
                atts.addElement(new Attribute(keys[i], labels));
            } else {
                // - otherwise we are assuming they are all numeric
                atts.addElement(new Attribute(keys[i]));
            }
        }
        return atts;
    }

    /**
     * grab a model file and deserialize it into a model
     * see https://weka.wikispaces.com/Serialization
     */
    public static Classifier unpack(String path) {
        try {
            ObjectInputStream ois = new ObjectInputStream(
                    new FileInputStream(path));
            Classifier c = (Classifier) ois.readObject();
            return c;

        } catch (Exception e) {
            e.printStackTrace();
            System.exit(1);
        }
        return null;
    }

    public static String getIp(String ip) {
        ip = ip.trim();
        // see https://docs.oracle.com/javase/tutorial/essential/regex/test_harness.html
        // in this case look at both strings at once as this is how they'll be sent
        Pattern ishostip = Pattern.compile("^\\w[\\w:\\.\\-]*\\w[/ ]+\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$");
        if (!ishostip.matcher(ip).find()) {
            System.err.println("invalid ip: "+ip);
            return null;
        }
        ip = ip.replaceAll(" ","/");
        if (verbose) System.err.println("searching for "+ip);
        return ip;
    }
        
    /**
     * consistent with other applications using the mod_ml db we're using 
     * the environment variables too
     */
    public static void dbSetup() {
        // see https://docs.oracle.com/javase/tutorial/essential/environment/env.html
        Map<String, String> env = System.getenv();
        dbuser = env.get("ML_DB_USER");
        dbpw = env.get("ML_DB_PW");
        dbname = env.get("ML_DB");
        dbhost = env.get("ML_DB_HOST");
        if (dbhost == null) dbhost = "localhost";
        dbport = env.get("ML_DB_PORT");
        if (dbport == null) dbport = "5432";
        dsn = "jdbc:postgresql://"+dbhost+":"+dbport+"/"+dbname;
        if (verbose) System.err.println("dsn: "+dsn);
    }

    /**
     * connect to postgres db using envvars with the relevant parameters
     */
    public static Connection dbConnect() {
        if (dsn == null) dbSetup();
        Connection conn = null;
        try {
            // see http://www.tutorialspoint.com/postgresql/postgresql_java.htm
            Class.forName("org.postgresql.Driver");
            conn = DriverManager.getConnection(dsn,dbuser,dbpw);
        } catch (Exception e) {
            System.err.println("connect failed for dsn "+dsn);
            System.err.println(
                    "do you have envvars ML_DB_HOST, ML_DB_PORT, "+
                    "ML_DB, ML_DB_USER and ML_DB_PW set?");
            System.exit(1);
        }
        return conn;
    }

    /**
     * try to find the stats record for this ip
     */
    public static Instances botstats(String ip) {
        FastVector atts;

        PreparedStatement st = null;
        ResultSet rs = null;
        Instances data = null;
        try {
            // from http://docs.oracle.com/javase/tutorial/jdbc/basics/prepared.html
            st = conn.prepareStatement(query);
            st.setString(1, ip);
            if (verbose) System.err.println(st);
            rs = st.executeQuery();
            if (rs.next()) {
                // for this application we don't really care what the actual designation is
                double[] row = new double[BOTSTATSZ];
                int cl = rs.getInt("class");
                int i = 0;
                row[i++] = (cl == 1 ? 1 : 0); 
                row[i++] = rs.getDouble("mean"); 
                row[i++] = rs.getDouble("var"); 
                row[i++] = rs.getDouble("skew"); 
                row[i++] = rs.getDouble("kurtosis"); 
                row[i++] = rs.getDouble("hmean"); 
                row[i++] = rs.getDouble("hvar"); 
                row[i++] = rs.getDouble("hskew"); 
                row[i++] = rs.getDouble("hkurtosis"); 
                row[i++] = rs.getDouble("htmean"); 
                row[i++] = rs.getDouble("htvar"); 
                row[i++] = rs.getDouble("htskew"); 
                row[i++] = rs.getDouble("htkurtosis"); 
                row[i++] = rs.getDouble("poverr"); 
                row[i++] = rs.getDouble("uacount"); 
                row[i++] = rs.getDouble("errprop"); 

                if (verbose) {
                    for (i = 0; i<row.length; i++) {
                        System.err.println(row[i]);
                    }
                }

                atts = mkAtts();
                data = new Instances("botstats", atts, 1);
                data.add(new Instance(1.0, row));

                // see https://weka.wikispaces.com/Use+Weka+in+your+Java+code#Classification-Classifying%20instances
                // "set class attribute" - predictions will fail w/o this
                data.setClassIndex(0);
                if (verbose) System.err.println(data);
            }

        } catch (Exception e) {
            // possible reason for a failure ...
            conn = dbConnect();
            // also from the tutorialspoint pg demo
            System.err.println(e.getClass().getName()+": "+e.getMessage());
            e.printStackTrace();

        } finally {
            // from http://www.mkyong.com/jdbc/jdbc-preparestatement-example-select-list-of-the-records/
            try {
                if (rs != null) { rs.close(); }
                if (st != null) { st.close(); }
            } catch (SQLException e) {
                // do nothing
            }
		}
        return data;
    }

    /**
     * read in an ip, search for it and run it through our classifiers
     * return the majority judgement
     */
    public String passJudgement(String ip) {

        if (ip == null) return "NOIP";

        // judgements have a lifetime now
        Judgement judgement = seen.get(ip);
        String cl = null;
        if (judgement != null) {
            cl = judgement.decJudgement();
            if (cl != null) {
                System.err.println("req "+reqcount+": cached: "+judgement);
                return cl;
            }
        }

        Instances data = botstats(ip);

        if (data != null) {
            Iterator iter = (Iterator) classifiers.iterator();
            double prediction = 0.0;
            double count = 0.0;
            while (iter.hasNext()) {
                Classifier c = (Classifier) iter.next();
                try { 
                    prediction += c.classifyInstance(data.instance(0));
                    count += 1.0;
                } catch (Exception e) {
                    System.err.println(e.getClass().getName()+": "+e.getMessage());
                }
            }
            if (prediction/count >= 0.5) {
                cl = "YES";
            } else {
                cl = "NO";
            }
        } else { 
            cl = "NOTFOUND";
        }
        judgement = new Judgement(ip, cl, 100);
        seen.put(ip, judgement);
        System.err.println("req "+reqcount+": new: "+judgement);
        return cl;
    }
        
    /**
     * make a socket connection on our port
     * then wait for input on it
     * print out the classification response
     */
    public void loop() {
        ServerSocket server = null;
        try {
            conn = dbConnect();
            server = new ServerSocket(port);
        } catch (Exception e) {
            System.err.println("server socket: "+e.getClass()+": "+e.getMessage());
            System.exit(1);
        }
        while (true) {
            try {
                reqcount = (reqcount + 1) % 2097152;
                new Loop(server.accept()).start();
            } catch (IOException e) {
                System.err.println("loop: "+e.getClass()+": "+e.getMessage());
            }
        }
    }
    /**
     * make some stuff for integrating database data with weka
     */
    public static void setKeys() {
        // this array is needed for making attributes for classification
        int i = 0;
        keys[i++] = "class";
        keys[i++] = "mean";
        keys[i++] = "var";
        keys[i++] = "skew";
        keys[i++] = "kurtosis";
        keys[i++] = "hmean";
        keys[i++] = "hvar";
        keys[i++] = "hskew";
        keys[i++] = "hkurtosis";
        keys[i++] = "htmean";
        keys[i++] = "htvar";
        keys[i++] = "htskew";
        keys[i++] = "htkurtosis";
        keys[i++] = "poverr";
        keys[i++] = "uacount";
        keys[i++] = "errprop";
    }
    /**
     * query needed to get data from stats table
     * note that some fields are compound
     */
    public static String mkQuery(String table) {
        String query = 
            "select "+
                "class,"+
                "mean,"+
                "var,"+
                "skew,"+
                "kurtosis,"+
                "hmean,"+
                "hvar,"+
                "hskew,"+
                "hkurtosis,"+
                "htmean,"+
                "htvar,"+
                "htskew,"+
                "htkurtosis,"+
                "pages/reqs as poverr,"+
                "array_length(uas,1) as uacount,"+
                "errs/reqs as errprop "+
                " from "+table+" where ip=? and mean is not null";
        return query;
    }

    /**
     * read the http_host and remote_addr from the command line
     * look up relevant stats data for my models
     * make weka compatible instance data for this if it exists
     * make a prediction
     * return the result
     */
    public static void main(String[] args) throws Exception {
        usage = usage + "\nthe models and stats table need to use the same number and type of fields";
        usage = usage + "\nthis query must work:\n"+mkQuery(stats);

        if (args.length < 3) {
            System.err.println(usage);
            System.exit(1);
        }

        port = Integer.parseInt(args[0]);

        stats = args[1];
        Pattern istable = Pattern.compile("^[a-zA-Z]\\w+$");
        if (!istable.matcher(stats).find()) {
            System.err.println("invalid table name: "+stats);
            System.err.println(usage);
            System.exit(1);
        }
        query = mkQuery(stats);

        System.err.println("Initializing classifiers...");
        classifiers = new ArrayList<Classifier>();
        for (int i=2; i<args.length; i++) {
            Classifier c = unpack(args[i]);
            if (c != null) {
                classifiers.add(c);
            }
        }
        if (classifiers.isEmpty()) {
            System.err.println("Unable to load any classifiers!");
            System.err.println(usage);
            System.exit(1);
        }
        System.err.println("Ready...");

        new BotClassifier().loop();
    }
}

