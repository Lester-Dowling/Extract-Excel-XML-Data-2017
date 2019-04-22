/**
 * @file   Simple-XML/Document.cpp
 * @date   Started 2019-04-18
 * @author Lester J. Dowling
 */
#include "pch-simple-xml.hpp"
#include "Simple-XML/Document.hpp"
#include "Simple-XML/Element-Creator.hpp"
#include "Simple-XML/Element-Visitor.hpp"
#include "Simple-XML/Element-Filter.hpp"
#include "Simple-XML/mini-grammar.hpp"
#include "Pseudo-XPath/Grade.hpp"
#include "Pseudo-XPath/mini-grammar.hpp"

namespace simple_xml {
	using std::runtime_error;
	using std::string;
	using std::vector;
	using std::cout;
	using std::endl;
	namespace ascii = boost::spirit::ascii;
	namespace qi = boost::spirit::qi;
	namespace f = boost::filesystem;
	using file_in_memory_t = std::vector<char>;
	using Memory_Iterator = file_in_memory_t::const_iterator;
	using Stream_Iterator = boost::spirit::basic_istream_iterator<char>;
	using String_Iterator = string::const_iterator;
	using XML_Grammar = mini_grammar<Memory_Iterator>;
	using XPath_Grammar = pseudo_xpath::mini_grammar<String_Iterator>;
	using Grade = pseudo_xpath::Grade;

	Document::Document(const boost::filesystem::path xml_path)
	{
		this->load_xml_file(xml_path);
	}


	void Document::load_xml_file(const f::path xml_path)
	{
		m_excel_xml_path = xml_path;
		if (!f::exists(m_excel_xml_path))
			throw runtime_error{ "No such file: " + m_excel_xml_path.string() };
		f::fstream xml_stream{ m_excel_xml_path };
		if (!xml_stream)
			throw runtime_error{ "Could not open file: " + m_excel_xml_path.string() };
		boost::system::error_code ec;
		const boost::uintmax_t xml_file_size = f::file_size(m_excel_xml_path, ec);
		if (ec || xml_file_size == static_cast<boost::uintmax_t>(-1))
			throw runtime_error{ "Failed to get XML file size." };

		file_in_memory_t file_in_memory;
		file_in_memory.reserve(xml_file_size);
		xml_stream.unsetf(std::ios::skipws); // No white space skipping!
		std::copy(
		  std::istream_iterator<char>(xml_stream),
		  std::istream_iterator<char>(),
		  std::back_inserter(file_in_memory));

		Memory_Iterator fitr = file_in_memory.begin();
		Memory_Iterator const fend = file_in_memory.end();
		Element_Creator element_creator{ this->m_elements };
		XML_Grammar xml_parser{ &element_creator };
		if (!phrase_parse(fitr, fend, xml_parser, ascii::space))
			throw runtime_error{ "Parsing failed." };

		extract_worksheet_titles();
	}


	static Grade::SP parse_xpath(const string xpath_text)
	{
		XPath_Grammar xpath_parser;
		String_Iterator sitr = xpath_text.begin();
		String_Iterator const send = xpath_text.end();
		if (!qi::phrase_parse(sitr, send, xpath_parser, ascii::space))
			throw runtime_error{ "Failed to parse this XPath: " + xpath_text };
		return xpath_parser.result;
	}


	void Document::extract_worksheet_titles()
	{
		m_filter.set_filter_path(parse_xpath("Workbook, Worksheet"));
		m_filter.visit_all_depth_first(
		  [&](Element_Visitor& visitor) -> bool //
		  {
			  Element& ele = visitor.current();
			  if (ele.name() == "Worksheet") {
				  if (ele.attribute("ss:Name").has_value()) {
					  m_titles.add_worksheet(ele.wkt_idx, *ele.attribute("ss:Name"));
				  }
				  else {
					  m_titles.add_worksheet(ele.wkt_idx, std::to_string(ele.wkt_idx));
				  }
			  }
			  return true;
		  });
	}


	void Document::extract_column_titles(int column_titles_row, int column_title_span)
	{
		m_column_titles_row = column_titles_row;
		m_column_title_span = column_title_span;
		for (const int wkt_idx : m_titles.wkt_indices()) {
			std::ostringstream titles_xpath_oss;
			titles_xpath_oss << "Workbook, Worksheet[" << wkt_idx << "], Table, "
							 << "Row[" << m_column_titles_row
							 << "],Cell,Data[ss:Type=String]";
			m_filter.set_filter_path(parse_xpath(titles_xpath_oss.str()));
			m_filter.visit_all_depth_first(
			  [&](Element_Visitor& visitor) -> bool //
			  {
				  Element& ele = visitor.current();
				  BOOST_ASSERT(ele.wkt_idx == wkt_idx);
				  BOOST_ASSERT(ele.row_idx == m_column_titles_row);
				  BOOST_ASSERT(ele.name() == "Data");
				  if (!ele.text().empty()) {
					  m_titles.add_col(wkt_idx, ele.col_idx, ele.text());
				  }
				  else {
					  m_titles.add_col(
						ele.wkt_idx, ele.col_idx, std::to_string(ele.col_idx));
				  }
				  return true;
			  });
		}
	}


	void Document::extract_row_titles(const std::string row_titles_column)
	{
		m_row_titles_column = row_titles_column;
		if (m_row_titles_column.empty())
			m_row_titles_column = "1";
		else {
			// Translate a single capital letter to a column number.  For example, given "-m
			// C" on the command line, translate that to "-m 3".
			if (m_row_titles_column.size() == 1) {
				if (std::isupper(m_row_titles_column.front())) {
					const int corresponding_column_number =
					  1 + static_cast<int>(m_row_titles_column.front()) -
					  static_cast<int>('A');
					m_row_titles_column = std::to_string(corresponding_column_number);
				}
			}
		}
		bool good_column_number = true;
		int column_number;
		try {
			column_number = std::stoi(m_row_titles_column);
		}
		catch (std::invalid_argument const&) {
			good_column_number = false;
		}

		const string col_name_filter{ "[Column=\"" + m_row_titles_column + "\"]" };
		const string col_number_filter{ "[Column=" + m_row_titles_column + "]" };
		const string& col_filter = good_column_number ? col_number_filter : col_name_filter;
		for (const int wkt_idx : m_titles.wkt_indices()) {
			std::ostringstream titles_xpath_oss;
			titles_xpath_oss << "Workbook,"									   //
							 << "Worksheet[" << wkt_idx << "], Table, "		   //
							 << "Row[Row>" << row_idx_start_of_data() << "], " //
							 << "Cell" << col_filter << ", Data[ss:Type=String]";
			m_filter.set_filter_path(parse_xpath(titles_xpath_oss.str()));
			m_filter.visit_all_depth_first(
			  [&](Element_Visitor& visitor) -> bool //
			  {
				  Element& ele = visitor.current();
				  BOOST_ASSERT(ele.wkt_idx == wkt_idx);
				  BOOST_ASSERT(ele.name() == "Data");
				  if (good_column_number) {
					  BOOST_ASSERT(ele.col_idx == column_number);
				  }
				  if (!ele.text().empty()) {
					  m_titles.add_row(wkt_idx, ele.row_idx, ele.text());
				  }
				  else {
					  m_titles.add_row(
						ele.wkt_idx, ele.row_idx, std::to_string(ele.row_idx));
				  }
				  return true;
			  });
		}
	}


	int Document::row_idx_start_of_data() const
	{
		BOOST_ASSERT(m_column_titles_row > 0);
		BOOST_ASSERT(m_column_title_span > 0);
		return m_column_titles_row + m_column_title_span - 1;
	}


	bool Document::one_data_visit(simple_xml::Element_Visitor& visitor)
	{
		m_one_data = visitor.current().text();
		return false;
	}


	std::string Document::extract_single_text(Grade::SP xpath_root)
	{
		m_one_data.clear();
		m_filter.set_filter_path(xpath_root)
		  .visit_all_depth_first(boost::bind(&Document::one_data_visit, this, _1));
		return m_one_data;
	}


	bool Document::write_all_fields_visit(simple_xml::Element_Visitor& visitor)
	{
		using std::cout;
		using std::endl;
		using std::setw;
		if (visitor.current().name() != "Data")
			return true;
		if (!visitor.current().name().empty()) {
			cout << setw(-12) << "Tag:" << ' ' << visitor.current().name() << endl;
			cout << setw(12) << "Depth:" << ' ' << visitor.depth() << endl;
			cout << setw(12) << "Path:" << ' ' << visitor.path_to_string() << endl;
		}
		if (!visitor.current().attributes.empty()) {
			cout << setw(12) << "Attr:" << ' ';
			for (auto attr : visitor.current().attributes) {
				cout << attr.first << '=' << attr.second << ' ' << ' ' << ' ';
			}
			cout << endl;
		}
		if (!visitor.current().text().empty()) {
			cout << setw(12) << "Text:" << ' ' << visitor.current().text() << endl;
		}
		cout << setw(12) << "Row:" << ' ' << visitor.current().row_idx << endl;
		cout << setw(12) << "Col:" << ' ' << visitor.current().col_idx << endl;
		if (const auto row_title =
			  m_titles.row_title(visitor.current().wkt_idx, visitor.current().row_idx);
			row_title.has_value()) //
		{
			cout << setw(12) << "Row Title:" << ' ' << *row_title << endl;
		}
		if (const auto col_title = m_titles.col_title(visitor.current().wkt_idx, visitor.current().col_idx);
			col_title.has_value()) //
		{
			cout << setw(12) << "Col Title:" << ' ' << *col_title << endl;
		}
		return true;
	}


	void Document::write_all_fields()
	{
		Element_Visitor v{ m_elements };
		v.visit_all_depth_first(boost::bind(&Document::write_all_fields_visit, this, _1));
	}

} // namespace simple_xml
